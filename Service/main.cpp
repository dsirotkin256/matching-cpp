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
#include <boost/circular_buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <cds/algo/atomic.h>
#include <cds/details/allocator.h>
#include <cds/gc/hp.h> // for cds::HP (Hazard Pointer) SMR
#include <cds/init.h>  // for cds::Initialize and cds::Terminate
#include <cds/user_setup/allocator.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <typeinfo>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <pqxx/pqxx>
#include <sstream>
#include <vector>

using namespace matching_engine;
using namespace std::chrono_literals;
using ns = std::chrono::nanoseconds;

using boost::asio::ip::tcp;
namespace http = boost::beast::http;
using request_t = http::request<boost::beast::http::string_body>;
using response_t = http::response<boost::beast::http::string_body>;

std::atomic_ullong id = 0;
double sigma_vol = 0.076;
double price = 0;

std::mutex m;
std::condition_variable cv;

typedef cds::urcu::gc<cds::urcu::general_buffered_stripped> rcu_gb;
struct BronsonAVLTreeMapTraits
    : public cds::container::bronson_avltree::make_traits<
          cds::container::bronson_avltree::relaxed_insert<false>,
          cds::opt::item_counter<cds::atomicity::cache_friendly_item_counter>>::
          type {};
typedef cds::container::BronsonAVLTreeMap<rcu_gb, std::string, double,
                                  BronsonAVLTreeMapTraits> map;
class DB {
private:
  std::unique_ptr<pqxx::connection> conn_;
  std::shared_ptr<spdlog::logger> logger_;

public:
  DB(const std::string host, const std::string port, const std::string dbname,
     const std::string user, const std::string password,
     const std::shared_ptr<spdlog::logger> logger)
      : logger_{logger} {
    std::string conn_info_{"host=" + host + " port=" + port + " user=" + user +
                           " password=" + password + " dbname=" + dbname};
    conn_ = std::make_unique<pqxx::connection>(conn_info_);
    logger_->info("Connected to {}", conn_->dbname());
  }
  DB(const DB &) = delete;
};

class TcpConnectionHandler
    : public std::enable_shared_from_this<TcpConnectionHandler> {
public:
  TcpConnectionHandler(boost::asio::io_context &ioc,
                       const std::shared_ptr<OrderBook> &ob,
                       const std::shared_ptr<spdlog::logger> logger)
      : strand_{std::make_unique<boost::asio::io_context::strand>(ioc)},
        socket_{ioc}, work_{ioc}, ob_{ob}, logger_{logger} {}

  void dispatch() {
    auto self = shared_from_this();
    http::async_read(
        socket_, buffer_, request_,
        [this, self](boost::system::error_code ec, std::size_t) {
          if (ec == boost::beast::http::error::end_of_stream)
            return;
          if (ec) {
            logger_->error("HTTP async read: {}", ec.message());
            return;
          }
          logger_->info("HTTP async read: {}", request_.target().to_string());

          std::string target = request_.target().to_string();
          boost::trim_if(target, [](auto ch) { return ch == '/'; });
          std::vector<std::string> params;
          boost::split(params, target, [](auto ch) { return ch == '/'; },
                       boost::token_compress_on);

          std::ostringstream ss;

          if (params[0] == "") {
            ss << "Matching service is up<br>";
            self->strand_->dispatch(
                [self, res = ss.str()] { self->reply(res); });
            return;
          }

          if (params[0] == "config" and params[1] == "vol") {
            Price move_by = std::stod(params[2]);
            sigma_vol *= move_by;
            ss << "Request: " << target << "<br>"
               << "Vol: " << sigma_vol << "<br>";
            self->strand_->dispatch(
                [self, res = ss.str()] { self->reply(res); });
            return;
          }

          if (params.size() < 3) {
            logger_->warn("HTTP async read: invalid request");
            ss << "Invalid request" << std::endl;
          } else {
            SIDE side = params[0] == "BUY" ? SIDE::BUY : SIDE::SELL;
            Price price = std::stod(params[1]);
            Price quantity = std::stod(params[2]);
            auto order = std::make_shared<Order>(std::to_string(id++), price,
                                                 quantity, side);
            auto start = Time::now();
            bool result = ob_->match(order);
            auto elapsed = Time::now() - start;
            ss << "Request: " << target << "<br>"
               << "Order: {" << *order << "}<br>"
               << "Matching result: " << result << "<br>"
               << "Elapsed time: " << elapsed.count() / 1e+6 << " ms. <br>";
          }
          self->strand_->dispatch([self, res = ss.str()] { self->reply(res); });
        });
  }

  tcp::socket &socket() { return socket_; }

  void start() {
    strand_->dispatch([self = shared_from_this()] { self->dispatch(); });
  }

private:
  response_t static build_response(http::status status, std::string msg,
                                   http::request<http::string_body> &req) {
    response_t res{status, req.version()};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = std::move(msg);
    res.prepare_payload();
    return res;
  }

  void reply(std::string body) {
    auto self = shared_from_this();
    auto response = std::make_shared<response_t>(
        build_response(http::status::ok, body, request_));
    http::async_write(
        socket_, *response,
        [this, self, response](boost::system::error_code ec, std::size_t) {
          if (ec) {
            logger_->error("HTTP async_write: {}", ec.message());
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
  std::shared_ptr<OrderBook> ob_;
  std::shared_ptr<spdlog::logger> logger_;
};

class MatchingServer {
public:
  MatchingServer(boost::asio::io_context &ioc, const short &port,
                 const std::shared_ptr<OrderBook> ob,
                 const std::shared_ptr<spdlog::logger> logger)
      : ioc_{ioc}, acceptor_{ioc, tcp::endpoint(tcp::v4(), port)}, ob_{ob},
        logger_{logger} {
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.listen(boost::asio::socket_base::max_listen_connections);
    accept();
  }

private:
  void accept() {
    auto conn = std::make_shared<TcpConnectionHandler>(ioc_, ob_, logger_);
    acceptor_.async_accept(
        conn->socket(), [this, conn](boost::system::error_code ec) {
          logger_->info("HTTP async accpet: {}",
                        boost::lexical_cast<std::string>(
                            conn->socket().remote_endpoint()));
          if (ec)
            logger_->error("HTTP async accept: {}", ec.message());
          else
            conn->dispatch();

          accept();
        });
  }

  tcp::acceptor acceptor_;
  boost::asio::io_context &ioc_;
  std::shared_ptr<OrderBook> ob_;
  std::shared_ptr<spdlog::logger> logger_;
};

int main(int argc, char *argv[]) {
  // Initialize libcds
  cds::Initialize();

  // Initialize RCU gc
  rcu_gb RCU_gb;
  map tree;
  // If main thread uses lock-free containers
  // the main thread should be attached to libcds infrastructure
  cds::threading::Manager::attachThread();

  boost::thread_group workers;
  spdlog::init_thread_pool(32768, std::thread::hardware_concurrency());
  auto console =
      spdlog::create_async<spdlog::sinks::stdout_color_sink_mt>("console");
  auto ob = std::make_shared<OrderBook>();
  try {
    boost::asio::io_context ioc;
    auto port = 8989;
    /* auto rolling_feed = boost::circular_buffer<std::tuple<Price,
     * Price>>(1000); */
    MatchingServer s(ioc, port, ob, console);
    console->info("Matching Service is running on port {}", port);

    DB db("db", "5432", "postgres", "postgres", "postgres", console);

    //    auto trade = [&]() {
    //      Price balance{50};
    //      std::vector<Price> op;
    //      while (true) {
    //        Price b_sum{0}, s_sum{0};
    //        Price b_avg{0}, s_avg{0};
    //        for (auto [b, s] : rolling_feed) {
    //          b_sum += b;
    //          s_sum += s;
    //        }
    //        b_avg = b_sum / rolling_feed.size();
    //        s_avg = s_sum / rolling_feed.size();
    //        auto [buy_price, sell_price] = rolling_feed.front();
    //        if (!op.empty() and buy_price > s_avg) {
    //          balance += buy_price * 0.05 * op.size();
    //          op.clear();
    //        } else if (sell_price < b_avg and balance > sell_price) {
    //          op.push_back(sell_price);
    //          balance -= sell_price * 0.05;
    //        }
    //        console->info("Buy price: {}, Sell price: {}, Avg sell price: {},
    //        Avg "
    //                      "buy price: {}, Balance: {}, Open "
    //                      "positions: {}",
    //                      buy_price, sell_price, s_avg, b_avg, balance,
    //                      std::accumulate(op.begin(), op.end(), 0.0) * 0.05);
    //        std::this_thread::sleep_for(1s);
    //      }
    //    };
    //
    /* auto simulate = [&]() { */
    double mu = 0;
    double T = 1;
    int steps = 1e+6 - 1;

    /* std::unique_lock<std::mutex> lk(m); */
    /* cv.wait(lk, [] { return price; }); */
    for (auto i = std::thread::hardware_concurrency(); i > 0; i--) {
      std::thread([&]() {
        // Attach the thread to libcds infrastructure
        cds::threading::Manager::attachThread();
        std::vector<double> GBM = geoBrownian(1.0, mu, sigma_vol, T, steps);
        ns sample_elapsed = 0ns;
        ns avg_elapsed = 0ns;
        for (auto price : GBM) {
          auto start = Time::now();
          tree.insert(std::to_string(price), price);
          /* auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL; */
          /* auto p = ceil(price * 10000) / 10000; */
          /* auto q = double(rand() % 10 + 1) / (rand() % 20 + 1); */
          /* auto order = */
          /*     std::make_shared<Order>(std::to_string(id++), p, q, side); */
          /* ob->match(order); */
          auto elapsed = Time::now() - start;
          sample_elapsed += elapsed;
        }
        console->info("Time elapsed: {} sec.", sample_elapsed.count() / 1e+9);
        console->info("Sample size: {}", GBM.size());
        // Detach thread when terminating
        cds::threading::Manager::detachThread();
      })
          .detach();
    };
    /* }; */
    //    auto ticker = [&]() {
    //      auto tick_logger =
    //          spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
    //              "tick_log", "feed.csv", true);
    //      tick_logger->set_pattern("%E.%F,%v");
    //      while (true) {
    //        auto bb = ob->best_buy();
    //        auto bs = ob->best_sell();
    //        if (bb > 0.0 or bs > 0.0) {
    //          tick_logger->info("{},{}", bb, bs);
    //          rolling_feed.push_front({bb, bs});
    //        }
    //        tick_logger->flush();
    //        std::this_thread::sleep_for(300ms);
    //      }
    //    };
    auto snapshot = [&]() {
        cds::threading::Manager::attachThread();
      while (true) {
        /* Destroy existing snapshot file on every iteration */
        /* auto ob_logger = */
        /*     spdlog::create_async<spdlog::sinks::basic_file_sink_mt>( */
        /*         "orderbook_log", "snapshot.csv", true); */
        /* ob_logger->set_pattern("%v"); */
        /* for (auto point : ob->snapshot()) { */
        /*   ob_logger->info("{},{},{},{}", point.side, point.price, */
        /*                   point.cumulative_quantity, point.size); */
        /* } */
        /* ob_logger->flush(); */
        /* spdlog::drop("orderbook_log"); */
        std::this_thread::sleep_for(5s);
        map::key_type max_key, min_key;
        auto max_val = tree.extract_max_key(max_key);
        auto min_val = tree.extract_min_key(min_key);
        console->info("Tree size: {}, Min: {}, Max: {}", tree.size(), *min_val, *max_val);
        tree.update(min_key,[&](bool is_new, std::string const& key, double& item) { item = *min_val; }, true);
        tree.update(max_key,[&](bool is_new, std::string const& key, double& item) { item = *max_val; }, true);
      }
    };
    //    auto price_seed = [&]() {
    //      std::ifstream cpuinfo;
    //      while (true) {
    //        cpuinfo.open("/proc/loadavg");
    //        if (cpuinfo.is_open()) {
    //          std::string line;
    //          while (getline(cpuinfo, line)) {
    //            std::vector<std::string> info;
    //            boost::split(info, line, [](auto ch) { return ch == ' '; },
    //                         boost::token_compress_on);
    //            price = std::stod(info[0]);
    //            cv.notify_one();
    //          }
    //        }
    //        cpuinfo.close();
    //        std::this_thread::sleep_for(1s);
    //      }
    //    };
    /* std::thread(simulate).detach(); */
    //    std::thread(ticker).detach();
    std::thread(snapshot).detach();
    //    std::thread(trade).detach();
    //    std::thread(price_seed).detach();

    /* for (unsigned i = std::thread::hardware_concurrency() - 1; i > 0; i--) */
    /*   workers.create_thread(boost::bind(&boost::asio::io_context::run,
     * &ioc)); */

    ioc.run();
  } catch (std::exception &e) {
    console->error(e.what());
  }

  /* std::cout << "Tree statistics: " << tree.statistics() << std::endl; */

  workers.join_all();

  // Terminate libcds
  cds::Terminate();

  return 0;
}