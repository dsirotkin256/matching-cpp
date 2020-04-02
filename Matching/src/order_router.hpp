#pragma once

#include <orderbook.hpp>
#include "influxdb.hpp"
#include "spdlog/spdlog.h"
#include <future>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <map>
#include <folly/concurrency/UnboundedQueue.h>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
//#include <boost/lockfree/queue.hpp>

namespace matching_engine {
  namespace router {
    class consumer {
      using ms = std::chrono::milliseconds;
      using ns = std::chrono::nanoseconds;
      public:
        consumer(std::shared_ptr<spdlog::logger> console):
          should_exit_{false}, console_(console) {}
        consumer(const consumer&) = delete;
        consumer() = delete;
        void shutdown() {
          should_exit_ = true;
        }
        void push(OrderPtr order) {
          queue_.enqueue(std::move(order));
        }
        void register_market(std::string market) {
          markets.emplace(market, market);
        }
        void listen() {
          for (const auto& [name, _] : markets) {
            console_->info("Consumer of {} started @{}", name, (pid_t) syscall (SYS_gettid));
          }
          OrderPtr order;
          auto last_log = Time::now();
          while(should_consume_()) {
            queue_.dequeue(order);
            auto &&ob = markets.at(order->market_name());
            const auto start = Time::now();
            ob.match(std::move(order));
            auto elapsed = Time::now() - start;
            /* Post consumer stats */
            if (std::chrono::duration_cast<ms>(start.time_since_epoch()).count() - std::chrono::duration_cast<ms>(last_log.time_since_epoch()).count() >= 250) {
              std::async(std::launch::async,[&,market=ob.market_name(),elapsed] { /* NOTE Does it actually make sense to use async here?*/
                  influxdb_cpp::builder()
                  .meas("order_matcher")
                  .tag("language", "c++")
                  .tag("service", "matching")
                  .tag("market", market)
                  .field("execution_duration", std::chrono::duration_cast<ns>(elapsed).count())
                  .field("consumer_queue_length", (long)queue_.size())
                  .timestamp(std::chrono::duration_cast<ns>(Time::now().time_since_epoch()).count())
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
        std::unordered_map<std::string, OrderBook> markets;
        folly::UnboundedQueue<OrderPtr, true, true, true, 16> queue_;
        //boost::lockfree::queue<OrderPtr> queue_;
        std::atomic_bool should_exit_;
        std::shared_ptr<spdlog::logger> console_;
    };

    class dispatcher {
      public:
        dispatcher() = default;
        dispatcher(const dispatcher&) = delete;
        dispatcher(std::shared_ptr<boost::asio::thread_pool> pool,
            std::shared_ptr<spdlog::logger> console,
            std::vector<std::string> markets): console_{console} {
          const auto cores = std::thread::hardware_concurrency();
          const auto markets_per_core = uint64_t(markets.size() / cores);
          auto reminder = markets.size() % cores;
          std::vector<std::shared_ptr<consumer>> consumer_pool;
          for (unsigned int core = 0; core < cores; core++) {
            consumer_pool.emplace_back(std::make_shared<consumer>(console_));
            auto& market_consumer = consumer_pool.back();
            if (reminder > 0) {
              auto market = markets.back();
              market_registry_[market] = market_consumer;
              market_consumer->register_market(market);
              markets.pop_back();
              --reminder;
            }
            /* Costruct consumer for N markets distributed evenly across logical cores */
            for (auto index = markets_per_core; index > 0; --index) {
              auto market = markets.back();
              market_registry_[market] = market_consumer;
              market_consumer->register_market(market);
              markets.pop_back();
            }
          }
          for (auto& c : consumer_pool) {
            boost::asio::post(*pool, std::bind(&consumer::listen, c));
          }
        }
        void send(OrderPtr order) {
          auto& market_consumer = market_registry_.at(order->market_name());
          market_consumer->push(std::move(order));
        }
      private:
        //tbb::concurrent_unordered_map<std::string, std::shared_ptr<consumer>> market_registry_;
        std::unordered_map<std::string, std::shared_ptr<consumer>> market_registry_;
        std::shared_ptr<spdlog::logger> console_;
    };
  } // namespace router
} // namespace matching_engine
