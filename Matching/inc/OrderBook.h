#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <shared_mutex>
#include <vector>
#include "tbb/parallel_reduce.h"
#include "tbb/blocked_range.h"
#include <boost/container/map.hpp>
#include <boost/container/deque.hpp>
#include <boost/container/adaptive_pool.hpp>
#include <boost/container/allocator.hpp>
#include <boost/container/node_allocator.hpp>
#include <boost/align/aligned_allocator.hpp>
#include <boost/align/aligned_delete.hpp>


namespace matching_engine {

  using Price = unsigned long;
  using Time = std::chrono::high_resolution_clock;
  using TimePoint = std::chrono::time_point<Time>;
  using UUID = unsigned long;

  enum SIDE { BUY, SELL };

  enum STATE { INACTIVE, ACTIVE, CANCELLED, FULFILLED };

  enum TIF { GTC };

  class Order {
    public:
      Order(const std::string &market_name, const UUID &uuid, const Price &price, const double &quantity,
          const SIDE &side, const STATE &state = STATE::INACTIVE,
          const TIF &tif = TIF::GTC, const double &executed_quantity = 0,
          const TimePoint &created = Time::now());
      Order() = delete;
      Order(const Order &) = delete;
      ~Order() = default;

      std::string market_name() const;
      UUID uuid() const;
      double quantity() const;
      Price price() const;
      SIDE side() const;
      void execute(const double &quantity);
      double leftover() const;
      TIF tif() const;
      void state(STATE state);
      bool is_buy() const;
      /* Price/Time priority */
      bool operator>=(const Order &rhs) const;
      bool operator==(const Order &rhs) const;
      /*
      friend std::ostream& operator<<(std::ostream &out, const Order &o) {
        return out << "Created: " << o.created_.time_since_epoch().count()
          << " , UUID: " << o.uuid_ << ", Side: " << o.side_
          << ", State: " << o.state_ << ", Price: " << o.price_
          << " Quantity: " << o.quantity_
          << ", Executed:  " << o.executed_quantity_
          << ", Leftover: " << o.leftover_;
      }
      */

    private:
      const std::string market_name_;
      const UUID uuid_;
      const Price price_;
      const double quantity_;
      const SIDE side_;
      const TimePoint created_;
      double executed_quantity_;
      double leftover_;
      STATE state_;
      TIF tif_;
  };

  using OrderPtr = std::unique_ptr<Order>;

  using queue_allocator = boost::container::allocator<OrderPtr>;
  using order_queue_type = boost::container::deque<OrderPtr, queue_allocator>;

  class OrderQueue
    : public order_queue_type {
      public:
        OrderQueue() = default;
        template <typename order_type>
          OrderQueue(order_type order);
        template <typename order_type, typename... args>
          OrderQueue(order_type order, args... rest);
        bool remove(UUID uuid);
        Price accumulate() const;
    };

  class OrderBook {
    private:
      struct Comp {
        enum compare_type { less, greater };
        explicit Comp(compare_type t) : type(t) {}
        template <typename lhs_type, typename rhs_type>
          bool operator()(const lhs_type &lhs, const rhs_type &rhs) const {
            return type == less ? lhs < rhs : lhs > rhs;
          }
        compare_type type;
      };
      const std::string market_name_;
      using map_allocator = boost::container::adaptive_pool<std::pair<const Price, OrderQueue>>;
      using order_tree_type = boost::container::map<Price, OrderQueue, Comp, map_allocator>;
    public:
      order_tree_type buy_tree_;
      order_tree_type sell_tree_;
      OrderBook(const OrderBook &) = delete;
      OrderBook();
      OrderBook(const std::string market_name);
      ~OrderBook() = default;
      bool cancel(const UUID uuid, SIDE side, Price price_node_hint);
      bool match(OrderPtr);
      Price best_buy() const;
      Price best_sell() const;
      Price quote() const;
      Price spread() const;
      std::string market_name() const;
      struct snapshot_point {
        Price price;
        double cumulative_quantity;
        unsigned long size;
        SIDE side;
      };
      std::vector<snapshot_point> snapshot() const;
  };

} // namespace matching_engine

