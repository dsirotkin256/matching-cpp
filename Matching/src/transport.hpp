#pragma once

#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/variant.hpp>
#include <nlohmann/json.hpp>
#include <order_router.hpp>
#include <bredis.hpp>

namespace matching_engine
{
namespace transport
{
namespace redis
{
using socket_t = boost::asio::ip::tcp::socket;
using endpoint_t = boost::asio::ip::tcp::endpoint;
using context_t = boost::asio::io_context;
namespace r = bredis;
using buffer = boost::asio::streambuf;
using iterator_t = typename r::to_iterator<buffer>::iterator_t;
using endpoint_t = boost::asio::ip::tcp::endpoint;
using tcp = boost::asio::ip::tcp;
using policy_t = r::parsing_policy::keep_result;
using parse_result_t = r::parse_result_mapper_t<iterator_t, policy_t>;
using read_callback_t = std::function<void(const boost::system::error_code &error_code, parse_result_t &&r)>;
using extractor_t = r::extractor<iterator_t>;

class client
{
public:
    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(context_t& ioc,
           const endpoint_t& endpoint,
           const std::shared_ptr<spdlog::logger>& console): console_{console}
    {
        socket_t socket{ioc, endpoint.protocol()};
        socket_t subscribtion_socket{ioc, endpoint.protocol()};
        socket.connect(endpoint);
        subscribtion_socket.connect(endpoint);
        c_ = std::make_shared< r::Connection<socket_t> >(std::move(subscribtion_socket));
        conn_ = std::make_shared< r::Connection<socket_t> >(std::move(socket));
        c_->write(r::single_command_t{psubscribe_cmd.data(), channel_name.data()});
        c_->async_read(b_, notification_handler_);
    };
private:
    const std::shared_ptr<spdlog::logger>& console_;
    std::shared_ptr<r::Connection<socket_t>> c_;
    std::shared_ptr<r::Connection<socket_t>> conn_;
    buffer b_;
    buffer queue_buff_;
    const std::string_view channel_name{"__keyspace@0__:CONSUMER"};
    const std::string_view queue_name{"CONSUMER"};
    const std::string_view processing_queue_name{"CONSUMER_PROCESSING"};
    const std::string_view psubscribe_cmd{"psubscribe"};
    const std::string_view pmessage_cmd{"pmessage"};
    const std::string_view lpush_cmd{"lpush"};
    const std::string_view rpoplpush_cmd{"rpoplpush"};

    read_callback_t notification_handler_ = [&](const boost::system::error_code& ec, parse_result_t &&r)
    {
        if (ec) {
            throw ec;
        }
        auto extract = boost::apply_visitor(extractor_t(), r.result);
        b_.consume(r.consumed);
        auto& replies = boost::get<r::extracts::array_holder_t>(extract);
        auto* type_reply = boost::get<r::extracts::string_t>(&replies.elements[0]);
        if (type_reply && type_reply->str.compare(psubscribe_cmd) == 0) {
            auto& channel = boost::get<r::extracts::string_t>(replies.elements[1]);
            auto& payload = boost::get<r::extracts::int_t>(replies.elements[2]);
            if (channel.str.compare(channel_name) == 0 && payload == 1) {
                console_->info("redis::subscribed to {}", channel_name);
            } else {
                console_->warn("redis::failed to subscribe to {}", channel_name);
            }
        } else if (type_reply && type_reply->str.compare(pmessage_cmd) == 0) {
            auto& payload = boost::get<r::extracts::string_t>(replies.elements[3]);
            if (payload.str.compare(lpush_cmd) == 0) {
                console_->info("redis::notification::new element in {}", channel_name);
                conn_->async_write(queue_buff_,
                                   r::single_command_t{rpoplpush_cmd.data(), queue_name.data(), processing_queue_name.data()},
                [&](const boost::system::error_code& ec, std::size_t bytes) {
                    if (ec) throw ec;
                    queue_buff_.consume(bytes);
                    conn_->async_read(queue_buff_, queue_handler_);
                });
            }
        }
        c_->async_read(b_, notification_handler_);
    };
    read_callback_t queue_handler_ = [&](const boost::system::error_code& ec, parse_result_t &&r)
    {
        if(ec) throw ec;
        auto extract = boost::apply_visitor(extractor_t(), r.result);
        queue_buff_.consume(r.consumed);
        auto &payload = boost::get<r::extracts::string_t>(extract);
        console_->info("redis::rpoplpush::{}", payload.str);
    };
};
} // namespace redis
namespace http
{
using boost::asio::ip::tcp;
namespace http = boost::beast::http;
using request_t = http::request<boost::beast::http::string_body>;
using response_t = http::response<boost::beast::http::string_body>;

class connection_handler
    : public std::enable_shared_from_this<connection_handler>
{
public:
    connection_handler(boost::asio::io_context &ioc,
                       const std::shared_ptr<router::dispatcher> dispatcher,
                       const std::shared_ptr<spdlog::logger>& console)
        : socket_{ioc}, strand_{std::make_unique<boost::asio::io_context::strand>(ioc)},
          work_{ioc}, dispatcher_{dispatcher}, console_{console} {}

    connection_handler(const connection_handler&) = delete;
    connection_handler& operator=(const connection_handler&) = delete;

    void dispatch()
    {
        auto self = shared_from_this();
        http::async_read(
            socket_, buffer_, request_,
        [this, self](boost::system::error_code ec, std::size_t) {
            if (ec == http::error::end_of_stream) {
                return;
            }
            if (ec) {
                console_->error("connection_handler::async_read: {}", ec.message());
                return;
            }
            //logger_->info("connection_handler::async_read: {}", request_.target().to_string());

            std::string_view target = request_.target();
            //boost::trim_if(target, [](auto ch) { return ch == '/'; });
            std::vector<std::string_view> params;
            //boost::split(params, target, [](auto ch) { return ch == '/'; },
            //    boost::token_compress_on);

            size_t first = 0;
            while (first < target.size()) {
                const auto second = target.find_first_of('/', first);
                if (first != second) params.emplace_back(target.substr(first, second-first));
                if (second == std::string_view::npos) break;
                first = second + 1;
            }

            //std::ostringstream ss;
            http::status status;
            if (params.size() < 4 or target.find(u8"favicon.ico") != std::string_view::npos) {
                console_->warn("connection_handler::async_read: Invalid request");
                //ss << nlohmann::json::parse("{\"target\":\""+target+"\",\"status\": \"FAILED\",\"origin\":\"" +
                //    boost::lexical_cast<std::string>(socket_.remote_endpoint()) + "\"}");
                status = http::status::bad_request;
            } else {
                SIDE side = params[0] == u8"BUY" ? SIDE::BUY : SIDE::SELL;
                std::string_view market = dispatcher_->registered_market_name(params[1]);
                Price price = std::stod(params[2].data());
                Quantity quantity = std::stod(params[3].data());
                dispatcher_->send(std::move(std::make_unique<Order>(market, side, price, quantity)));
                status = http::status::ok;
            }
            self->strand_->dispatch([self, status] { self->reply(status); });
        });
    }

    tcp::socket &socket()
    {
        return socket_;
    }

    void start()
    {
        strand_->dispatch([self = shared_from_this()] { self->dispatch(); });
    }

private:
    response_t static build_response(http::status status,
                                     http::request<http::string_body> &req)
    {
        response_t res{status, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "application/json");
        res.keep_alive(req.keep_alive());
        //res.body() = std::string(msg);
        res.prepare_payload();
        return res;
    }

    void reply(http::status status)
    {
        auto self = shared_from_this();
        auto response = std::make_shared<response_t>(
                            build_response(status, request_));
        http::async_write(
            socket_, *response,
        [this, self, response](boost::system::error_code ec, std::size_t) {
            if (ec) {
                console_->error("server::async_write: {}", ec.message());
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
    std::shared_ptr<router::dispatcher> dispatcher_;
    const std::shared_ptr<spdlog::logger>& console_;
};

class server
{
public:
    server(boost::asio::io_context &ioc,
           const std::shared_ptr<router::dispatcher> dispatcher,
           const std::shared_ptr<spdlog::logger> console,
           const short port = 8080,
           const uint64_t available_cores = std::max(1u, std::thread::hardware_concurrency() - 1))
        : ioc_{ioc}, acceptor_{ioc, tcp::endpoint(tcp::v6(), port)},
          dispatcher_{dispatcher}, console_{console}, pool_{available_cores}
    {
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        async_start_();
        /* Start event loop for all threads */
        for (auto core = available_cores; core > 0; --core) {
            boost::asio::post(pool_, [&] {ioc.run();});
        }
    }
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    boost::system::error_code shutdown()
    {
        boost::system::error_code ec;
        acceptor_.close(ec);
        return ec;
    }

private:
    void async_start_()
    {
        const auto endpoint = boost::lexical_cast<std::string>(acceptor_.local_endpoint());
        acceptor_.listen(boost::asio::socket_base::max_listen_connections);
        if (acceptor_.is_open()) {
            console_->info("server::start: started on {}", endpoint);
        } else {
            console_->warn("server::start: failed to start on {}", endpoint);
        }
        async_accept_();
    }
    void async_accept_()
    {
        auto handler = std::make_shared<connection_handler>(ioc_, dispatcher_, console_);
        acceptor_.async_accept(
        handler->socket(), [this, handler](boost::system::error_code ec) {
            //logger_->info("server::async_accept: {}",
            //    boost::lexical_cast<std::string>(
            //      handler->socket().remote_endpoint()));
            if (ec) {
                console_->error("server::async_accept: {}", ec.message());
            } else {
                handler->dispatch();
            }
            async_accept_();
        });
    }

    boost::asio::io_context &ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<router::dispatcher> dispatcher_;
    const std::shared_ptr<spdlog::logger> console_;
    boost::asio::thread_pool pool_;
};
} // namespace http
} // namespace transport
} // namespace matching_engine
