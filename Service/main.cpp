#include "OrderBook.h"
#include "markov.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/lexical_cast.hpp>
#include <cassert>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <vector>

#include <cds/container/basket_queue.h>
#include <cds/gc/hp.h>
#include <cds/init.h>

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
          ob_->print_summary();
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
  cds::Initialize();

  // attach this thread to CDS:
  cds::gc::hp::GarbageCollector::Construct();
  cds::threading::Manager::attachThread();

  auto logger = spdlog::stdout_color_mt("console");
  try {
    auto ob = std::make_shared<OrderBook>(logger);
    boost::asio::io_context ioc;
    auto port = 8080;
    MatchingServer s(ioc, port, ob, logger);
    logger->info("Matching Service is running on port {}", port);
    auto simulate = [&ob, &logger]() {
      double S0 = 0.04;
      double mu = 0.2;
      double sigma = 0.1;
      double T = 1;
      int steps = 1e+7 - 1;
      std::vector<double> GBM = geoBrownian(S0, mu, sigma, T, steps);
      ns sample_elapsed = 0ns;
      ns avg_elapsed = 0ns;
      for (auto price : GBM) {
        auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
        auto p = ceil(price * 10000) / 10000;
        auto q = double(rand() % 1000 + 1) / (rand() % 20 + 1);
        auto order = std::make_shared<Order>(std::to_string(id++), p, q, side);
        auto start = Time::now();
        ob->match(order);
        auto elapsed = Time::now() - start;
        sample_elapsed += elapsed;
      }
      ob->print_summary();
      logger->info("Time elapsed: {} sec.", sample_elapsed.count() / 1e+9);
      logger->info("Sample size: {}", GBM.size());
    };
    std::thread(simulate).detach();
    ioc.run();
  } catch (std::exception &e) {
    logger->error(e.what());
  }
  cds::Terminate();

  return 0;
}