#pragma once

#include "spdlog/spdlog.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <shared_mutex>
#include <vector>

namespace matching_engine {

using Price = long double;
using Time = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Time>;
using UUID = std::string;

enum SIDE { BUY, SELL };

enum STATE { INACTIVE, ACTIVE, CANCELLED, FULFILLED };

enum TIF { GTC };

class Order {
public:
  Order(const std::string &uuid, const Price &price, const Price &quantity,
        const SIDE &side, const STATE &state = STATE::INACTIVE,
        const TIF &tif = TIF::GTC, const Price &executed_quantity = 0,
        const TimePoint &created = Time::now());
  Order() = delete;
  Order(const Order &) = delete;
  ~Order() = default;

  Price quantity() const;
  Price price() const;
  SIDE side() const;
  void execute(const Price &quantity);
  Price leftover() const;
  TIF tif() const;
  void state(STATE state);
  bool is_buy() const;
  /* Price/Time priority */
  bool operator>=(const Order &rhs) const;
  bool operator==(const Order &rhs) const;
  friend std::ostream& operator<<(std::ostream &out, const Order &o) {
    return out << "Created: " << o.created_.time_since_epoch().count()
               << " , UUID: " << o.uuid_ << ", Side: " << o.side_
               << ", State: " << o.state_ << ", Price: " << o.price_
               << " Quantity: " << o.quantity_
               << ", Executed:  " << o.executed_quantity_
               << ", Leftover: " << o.leftover_;
}

private:
  const UUID uuid_;
  const Price price_;
  const Price quantity_;
  Price executed_quantity_;
  const SIDE side_;
  STATE state_;
  TIF tif_;
  const TimePoint created_;
  Price leftover_;
};

class OrderQueue
    : public std::priority_queue<std::shared_ptr<Order>,
                                 std::vector<std::shared_ptr<Order>>,
                                 std::greater<std::shared_ptr<Order>>> {
public:
  OrderQueue() = default;
  bool remove(std::shared_ptr<Order>&);
  Price accumulate() const;
};

class OrderBook {
private:
  struct Comp {
    enum compare_type { less, greater };
    explicit Comp(compare_type t) : type(t) {}
    template <class T, class U> bool operator()(const T &t, const U &u) const {
      return type == less ? t < u : t > u;
    }
  compare_type type;
  };
  mutable std::shared_mutex m_;
  std::shared_ptr<spdlog::logger> logger_;

public:
  using OrderTree = std::map<Price, OrderQueue, Comp>;
  OrderTree buy_tree_;
  OrderTree sell_tree_;
  OrderBook(const OrderBook &) = delete;
  OrderBook();
  OrderBook(const std::shared_ptr<spdlog::logger>&);
  ~OrderBook() = default;

  bool cancel(std::shared_ptr<Order>&);
  bool match(std::shared_ptr<Order>&);
  Price best_buy() const;
  Price best_sell() const;
  Price quote() const;
  Price spread() const;

  struct snapshot_point {
    Price price;
    Price cumulative_quantity;
    unsigned long size;
    SIDE side;
  };

  std::vector<snapshot_point> snapshot() const;
};

} // namespace matching_engine

