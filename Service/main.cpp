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
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
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
               << "Elapsed time: " << elapsed.count() / 1e+9 << " sec. <br>";
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
  spdlog::init_thread_pool(32768, std::thread::hardware_concurrency());
  auto console =
      spdlog::create_async<spdlog::sinks::stdout_color_sink_mt>("console");
  auto ob = std::make_shared<OrderBook>();
  try {
    boost::asio::io_context ioc;
    auto port = 8080;
    MatchingServer s(ioc, port, ob, console);
    console->info("Matching Service is running on port {}", port);

    auto simulate = [&]() {
      double S0 = 8.60;
      while (true) {
        double mu = double(rand() % 2 + 1) / 10'000 *
                    (rand() % 2 ? 1 : -1); // add rand drift (+/-)0.01-0.02%
        double sigma = 0.008 + double(rand() % 2 + 1) /
                                   1'000; // flat 0.8% vol + rand 0.1-0.2%
        double T = 1;
        int steps = 1e5 - 1;
        std::vector<double> GBM = geoBrownian(S0, mu, sigma, T, steps);
        ns sample_elapsed = 0ns;
        ns avg_elapsed = 0ns;
        for (auto price : GBM) {
          auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
          auto p = ceil(price * 10'000) / 10'000;
          auto q = double(rand() % 10 + 1) / (rand() % 20 + 1);
          auto order =
              std::make_shared<Order>(std::to_string(id++), p, q, side);
          auto start = Time::now();
          ob->match(order);
          auto elapsed = Time::now() - start;
          sample_elapsed += elapsed;
        }
        S0 = ob->best_sell();
        console->info("Time elapsed: {} sec.", sample_elapsed.count() / 1e+9);
        console->info("Sample size: {}", GBM.size());
        std::this_thread::sleep_for(50ms);
      }
    };
    auto ticker = [&]() {
      auto tick_logger =
          spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
              "tick_log", "feed.csv", true);
      tick_logger->set_pattern("%E%e,%v");
      auto pbs = ob->best_sell();
      auto pbb = ob->best_buy();
      while (true) {
        auto bb = ob->best_buy();
        auto bs = ob->best_sell();
        if ((bb && bs && pbs && pbb) && bs != pbs) {
          tick_logger->info("{},{}", bb, bs);
        }
        tick_logger->flush();
        pbb = bb;
        pbs = bs;
        std::this_thread::sleep_for(500ms);
      }
    };
    auto snapshot = [&]() {
      while (true) {
        // Destroy existing snapshot file on every iteration
        auto ob_logger =
            spdlog::create_async<spdlog::sinks::basic_file_sink_mt>(
                "orderbook_log", "snapshot.csv", true);
        ob_logger->set_pattern("%v");
        for (auto point : ob->snapshot()) {
          ob_logger->info("{},{},{},{}", point.side, point.price,
                          point.cumulative_quantity, point.size);
        }
        ob_logger->flush();
        spdlog::drop("orderbook_log");
        std::this_thread::sleep_for(500ms);
      }
    };
    std::thread(simulate).detach();
    std::thread(ticker).detach();
    std::thread(snapshot).detach();

    ioc.run();
  } catch (std::exception &e) {
    console->error(e.what());
  }

  return 0;
}