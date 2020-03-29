#include "OrderBook.h"
#include "markov.h"
#include "spdlog/async.h"
#include "spdlog/async_logger.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/lexical_cast.hpp>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <math.h>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>
#include "influxdb.hpp"
#include "tbb/concurrent_queue.h"
#include "tbb/concurrent_unordered_map.h"
#include <folly/concurrency/UnboundedQueue.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

using namespace matching_engine;
using namespace std::chrono_literals;
using ns = std::chrono::nanoseconds;

using boost::asio::ip::tcp;
namespace http = boost::beast::http;
using request_t = http::request<boost::beast::http::string_body>;
using response_t = http::response<boost::beast::http::string_body>;

std::atomic_ullong id = 0;

class market_consumer {
  public:
    market_consumer(const std::string& market_name):
      should_exit_{false}, ob_(market_name) {}
    market_consumer(const market_consumer&) = delete;
    void shutdown() {
      should_exit_ = true;
    }
    std::string market_name() const {
      return ob_.market_name();
    }
    void push(OrderPtr order) {
      queue_.enqueue(std::move(order));
    }
    void listen() {
      std::cout << "Consumer of " << ob_.market_name() << " started at T " << (pid_t) syscall (SYS_gettid) << std::endl;
      OrderPtr order;
      auto last_log = Time::now();
      while(should_consume_()) {
        queue_.dequeue(order);
        auto start = Time::now();
        ob_.match(std::move(order));
        auto elapsed = Time::now() - start;
        /* Post consumer stats */
        if (std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count() - std::chrono::duration_cast<std::chrono::milliseconds>(last_log.time_since_epoch()).count() >= 250) {
          std::async(std::launch::async,[&,elapsed] {
          influxdb_cpp::builder()
            .meas("order_matcher")
            .tag("language", "c++")
            .tag("service", "matching")
            .tag("market", ob_.market_name())
            .field("execution_duration", std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())
            .field("consumer_queue_length", (long)queue_.size())
            .field("total_orders_processed", (long)id)
            .timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(Time::now().time_since_epoch()).count())
            .send_udp("172.17.0.1", 8089);
          });
          last_log = start;
        }
      }
    }
  private:
    bool should_consume_() const {
      return !should_exit_ or !queue_.empty();
    }
    OrderBook ob_;
    folly::UnboundedQueue<OrderPtr, true, true, true, 16> queue_;
    std::atomic_bool should_exit_;
};

class order_dispatcher {
  public:
    order_dispatcher(const order_dispatcher&) = delete;
    order_dispatcher() {}
    void register_market_consumer(std::shared_ptr<market_consumer> consumer) {
      market_registry_[consumer->market_name()] = std::move(consumer);
    }
    void send(OrderPtr order, bool relaxed = true) {
      auto& consumer = market_registry_.at(order->market_name());
      consumer->push(std::move(order));
    }
  private:
    tbb::concurrent_unordered_map<std::string, std::shared_ptr<market_consumer>> market_registry_;
};

class tcp_connection_handler
: public std::enable_shared_from_this<tcp_connection_handler> {
  public:
    tcp_connection_handler(boost::asio::io_context &ioc,
        const std::shared_ptr<order_dispatcher> dispatcher,
        const std::shared_ptr<spdlog::logger> logger)
      : strand_{std::make_unique<boost::asio::io_context::strand>(ioc)},
      socket_{ioc}, work_{ioc}, dispatcher_{dispatcher}, logger_{logger} {}

    tcp_connection_handler(const tcp_connection_handler&) = delete;

    void dispatch() {
      auto self = shared_from_this();
      http::async_read(
          socket_, buffer_, request_,
          [this, self](boost::system::error_code ec, std::size_t) {
          if (ec == http::error::end_of_stream) {
          return;
          }
          if (ec) {
          logger_->error("tcp_connection_handler::async_read: {}", ec.message());
          return;
          }
          //logger_->info("tcp_connection_handler::async_read: {}", request_.target().to_string());

          std::string target = request_.target().to_string();
          boost::trim_if(target, [](auto ch) { return ch == '/'; });
          std::vector<std::string> params;
          boost::split(params, target, [](auto ch) { return ch == '/'; },
              boost::token_compress_on);

          //std::ostringstream ss;
          http::status status;
          if (params.size() < 4 or target.find("favicon.ico") != std::string::npos) {
          logger_->warn("tcp_connection_handler::async_read: Invalid request");
          //ss << nlohmann::json::parse("{\"target\":\""+target+"\",\"status\": \"FAILED\",\"origin\":\"" +
          //    boost::lexical_cast<std::string>(socket_.remote_endpoint()) + "\"}");
            status = http::status::bad_request;
          } else {
            SIDE side = params[0] == "BUY" ? SIDE::BUY : SIDE::SELL;
            std::string market = params[1];
            Price price = std::stod(params[2]);
            double quantity = std::stod(params[3]);
            auto order_id = id++;
            dispatcher_->send(std::move(std::make_unique<Order>(market, order_id, price,
                    quantity, side)));
            status = http::status::ok;
            //ss << "{\"status\": \"OK\"}";
            /*ss << nlohmann::json::parse(
                "{\"target\":\"" + target + "\"," +
                "\"order_id\":" + std::to_string(order_id) + "," +
                "\"status\": \"OK\"," +
                "\"origin\":\"" + boost::lexical_cast<std::string>(socket_.remote_endpoint()) + "\"}");
            //"\"order_submitted\":" + std::to_string(was_sent) + "," +
            */
          }
          self->strand_->dispatch([self, status] { self->reply(status); });
          });
    }

    tcp::socket &socket() { return socket_; }

    void start() {
      strand_->dispatch([self = shared_from_this()] { self->dispatch(); });
    }

  private:
    response_t static build_response(http::status status,
        http::request<http::string_body> &req) {
      response_t res{status, req.version()};
      //res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
      res.set(http::field::content_type, "application/json");
      res.keep_alive(req.keep_alive());
      //res.body() = std::string(msg);
      res.prepare_payload();
      return res;
    }

    void reply(http::status status) {
      auto self = shared_from_this();
      auto response = std::make_shared<response_t>(
          build_response(status, request_));
      http::async_write(
          socket_, *response,
          [this, self, response](boost::system::error_code ec, std::size_t) {
          if (ec) {
          logger_->error("tcp_server::async_write: {}", ec.message());
          return;
          }
          if (response->need_eof())
          return;

          if (!ec)
          self->strand_->dispatch([self] { self->dispatch(); });
          });
    }

    tcp::socket socket_;
    std::unique_ptr<boost::asio::io_context::strand> strand_;
    boost::asio::io_context::work work_;
    boost::beast::flat_buffer buffer_;
    request_t request_;
    std::shared_ptr<order_dispatcher> dispatcher_;
    std::shared_ptr<spdlog::logger> logger_;
};

class tcp_server {
  public:
    tcp_server(boost::asio::io_context &ioc, const short port,
        const std::shared_ptr<order_dispatcher> dispatcher,
        const std::shared_ptr<spdlog::logger> logger = 
        spdlog::create_async<spdlog::sinks::stdout_color_sink_mt>("console"))
      : ioc_{ioc}, acceptor_{ioc, tcp::endpoint(tcp::v6(), port)},
      dispatcher_{dispatcher}, logger_{logger} {
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
      }
    tcp_server(const tcp_server&) = delete;

    void async_start() {
      const auto endpoint = boost::lexical_cast<std::string>(acceptor_.local_endpoint());
      acceptor_.listen(boost::asio::socket_base::max_listen_connections);
      if (acceptor_.is_open()) {
        logger_->info("tcp_server::start: started on {}", endpoint);
      } else {
        logger_->warn("tcp_server::start: failed to start on {}", endpoint);
      }
      async_accept();
    }
    boost::system::error_code shutdown() {
      boost::system::error_code ec;
      acceptor_.close(ec);
      return ec;
    }

  private:
    void async_accept() {
      auto handler = std::make_shared<tcp_connection_handler>(ioc_, dispatcher_, logger_);
      acceptor_.async_accept(
          handler->socket(), [this, handler](boost::system::error_code ec) {
          //logger_->info("tcp_server::async_accept: {}",
          //    boost::lexical_cast<std::string>(
          //      handler->socket().remote_endpoint()));
          if (ec) {
          logger_->error("tcp_server::async_accept: {}", ec.message());
          } else {
          handler->dispatch();
          }
          async_accept();
          });
    }

    tcp::acceptor acceptor_;
    boost::asio::io_context &ioc_;
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<order_dispatcher> dispatcher_;
};

int main(int argc, char *argv[]) {
  /* Initialise logging service */
  spdlog::init_thread_pool(32768, std::thread::hardware_concurrency());
  spdlog::flush_every(std::chrono::seconds(1));
  const auto console =
    spdlog::create_async<spdlog::sinks::stdout_color_sink_mt>("console");

  /* Initialise order dispatching service */
  auto pool = boost::asio::thread_pool(std::thread::hardware_concurrency()+100);
  std::vector<std::string> markets({"BTC_USD", "EUR_GBP", "AUD_USD", "GBP_USD", "NZD_USD", "USD_CHF", "EUR_AUD", "GBP_JPY", "USD_JPY"});
  auto dispatcher = std::make_shared<order_dispatcher>();
  for (const auto& market : markets) {
    auto consumer = std::make_shared<market_consumer>(market);
    dispatcher->register_market_consumer(consumer);
    /* Activate consumer */
    boost::asio::post(pool, std::bind(&market_consumer::listen, consumer));
  }

  /* Initialise TCP transport layer */
  boost::asio::io_context ioc{(int)std::thread::hardware_concurrency()};
  tcp_server server(ioc, 8080, dispatcher, console);
  server.async_start();

  const auto produce_orders = [&](std::string market) {
    double S0 = 80;
    while (true) {
      double mu = double(rand() % 5 + 1) / 100 *
        (rand() % 2 ? 1 : -1); // add rand drift
      double sigma = 0.08 + double(rand() % 2 + 1) /
        1000; // flat vol + rand
      double T = 1;
      int steps = 1e6 - 1;
      std::vector<double> GBM = geoBrownian(S0, mu, sigma, T, steps);
      for (auto price : GBM) {
        auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
        auto q = double(rand() % 10 + 1) / (rand() % 20 + 1);
        auto order = std::make_unique<Order>(market, id++, price, q, side);
        dispatcher->send(std::move(order));
      }
    }
  };
  /*
     const auto tick_writer = [&] {
     auto tick_logger =
     spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
     "tick_log", "data/feed.csv", true);
     tick_logger->set_pattern("%E%e,%v");
     auto pbs = ob->best_sell();
     auto pbb = ob->best_buy();
     while (true) {
     auto bb = ob->best_buy();
     auto bs = ob->best_sell();
     if ((bb && bs && pbs && pbb) && bs != pbs) {
     tick_logger->info("{},{}", bb, bs);
     }
     pbb = bb;
     pbs = bs;
     std::this_thread::sleep_for(500ms);
     }
     };
     */
  /*
     const auto snapshot_writer = [&] {
     while (true) {
  // Destroy existing snapshot file on every iteration
  auto ob_logger =
  spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
  "orderbook_log", "data/snapshot.csv", true);
  ob_logger->set_pattern("%v");
  for (auto point : ob->snapshot()) {
  ob_logger->info("{},{},{},{}", point.side, point.price,
  point.cumulative_quantity, point.size);
  }
  spdlog::drop("orderbook_log");
  std::this_thread::sleep_for(500ms);
  }
  };
  */
  //boost::asio::post(pool, tick_writer);
  //boost::asio::post(pool, snapshot_writer);

  for (const auto& market : markets)
    boost::asio::post(pool, [&]{produce_orders(market);});

  boost::asio::post(pool, [&]{ioc.run();});
  boost::asio::post(pool, [&]{ioc.run();});
  boost::asio::post(pool, [&]{ioc.run();});
  ioc.run();

  pool.join();

  return 0;
}