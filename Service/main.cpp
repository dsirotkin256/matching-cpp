#include "OrderBook.h"
#include "markov.h"
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

using namespace matching_engine;
using namespace std::chrono_literals;
using ns = std::chrono::nanoseconds;

using boost::asio::ip::tcp;
namespace http = boost::beast::http;
using request_t = http::request<boost::beast::http::string_body>;
using response_t = http::response<boost::beast::http::string_body>;

static unsigned long long id = 0;

class TcpConnectionHandler
    : public std::enable_shared_from_this<TcpConnectionHandler> {
public:
  TcpConnectionHandler(boost::asio::io_context &ioc,
                       const std::shared_ptr<OrderBook> &ob)
      : strand_{std::make_unique<boost::asio::io_context::strand>(ioc)},
        socket_{ioc}, work_{ioc}, ob_{ob} {}

  void dispatch() {
    auto self = shared_from_this();
    http::async_read(
        socket_, buffer_, request_,
        [this, self](boost::system::error_code ec, std::size_t) {
          if (ec == boost::beast::http::error::end_of_stream)
            return;
          if (ec) {
            std::cerr << "http::async_read: " << ec << std::endl;
            return;
          }
          std::cout << "Received: " << request_.body() << std::endl;

          std::vector<std::string> params;
          boost::split(params, request_.body(), boost::is_any_of(","));
          if (params.size() < 3) {
            throw std::invalid_argument("invalid arguments");
          }
          SIDE side = params[0] == "BUY" ? SIDE::BUY : SIDE::SELL;
          Price price = std::stod(params[1]);
          Price quantity = std::stod(params[2]);
          auto order = std::make_shared<Order>(std::to_string(id++), price,
                                               quantity, side);

          ob_->match(order);
          self->strand_->dispatch([self] { self->reply(); });
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

  void reply() {
    auto self = shared_from_this();
    auto response = std::make_shared<response_t>(
        build_response(http::status::ok, request_.body(), request_));
    http::async_write(
        socket_, *response,
        [this, self, response](boost::system::error_code ec, std::size_t) {
          if (ec) {
            std::cerr << "http::async_write: " << ec << std::endl;
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
};

class MatchingServer {
public:
  MatchingServer(boost::asio::io_context &ioc, const short &port,
                 const std::shared_ptr<OrderBook> ob)
      : ioc_{ioc}, acceptor_{ioc, tcp::endpoint(tcp::v4(), port)}, ob_{ob} {
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.listen(boost::asio::socket_base::max_listen_connections);
    accept();
  }

private:
  void accept() {
    auto conn = std::make_shared<TcpConnectionHandler>(ioc_, ob_);
    acceptor_.async_accept(
        conn->socket(), [this, conn](boost::system::error_code ec) {
          std::cout << "New connection: " << conn->socket().remote_endpoint()
                    << std::endl;
          if (ec)
            std::cerr << "http::async_write: " << ec << std::endl;
          else
            conn->dispatch();

          accept();
        });
  }

  tcp::acceptor acceptor_;
  boost::asio::io_context &ioc_;
  std::shared_ptr<OrderBook> ob_;
};

int main(int argc, char *argv[]) {
  try {
    auto ob = std::make_shared<OrderBook>();
    boost::asio::io_context ioc;
    auto port = 8080;
    MatchingServer s(ioc, port, ob);
    std::cout << "Matching Service is running on port " << port << std::endl;
    std::thread([&ob] {
      double S0 = 0.04;
      double mu = 0.5;
      double sigma = 0.1;
      double T = 1;
      int steps = 1e+6 - 1;
      while (true) {
        std::vector<double> GBM = geoBrownian(S0, mu, sigma, T, steps);
        ns elapsed = 0ns;
        for (auto price : GBM) {
          auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
          auto p = ceil(price * 10000) / 10000;
          auto q = double(rand() % 1000 + 1) / (rand() % 20 + 1);
          auto order =
              std::make_shared<Order>(std::to_string(id++), p, q, side);
          auto start = Time::now();
          ob->match(order);
          elapsed += Time::now() - start;
        }

        std::cout << "\n\n========== Sample Summary =========\n\n";
        std::cout << "Time elapsed: " << elapsed.count() / 1e+9 << " sec.\n";
        std::cout << "Sample size: " << GBM.size() << "\n\n";
        std::this_thread::sleep_for(5s);
      }
    })
        .detach();
    std::thread([&ob] {
      while (true) {
        ob->print_summary();
        std::this_thread::sleep_for(2s);
      }
    })
        .detach();
    ioc.run();
  } catch (std::exception &e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}