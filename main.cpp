#include "markov.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <vector>

namespace matching_engine {

using Price = double;
using Time = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Time>;
using UUID = std::string;

enum SIDE { BUY, SELL };

enum STATE { INACTIVE, ACTIVE, CANCELLED, FULFILLED };

enum TIF { GTC };

double fee_income = 0.0;

class Order {
public:
  Order()
      : uuid_{}, price_{0}, quantity_{0}, executed_quantity_{0},
        state_{STATE::INACTIVE}, created_{Time::now()} {};
  Order(const Order &) = delete;
  ~Order() = default;
  UUID uuid_;
  Price price_;
  double quantity_;
  double executed_quantity_;
  SIDE side_;
  STATE state_;
  TIF tif_;
  TimePoint created_;
  Price leftover() const { return quantity_ - executed_quantity_; }
  void state(STATE state) {
    state_ = state;
    if (state == STATE::FULFILLED) {
      fee_income += quantity_ * 0.002;
    }
  }
  /* Price/Time priority */
  bool operator<=(const Order &rhs) const {
    return side_ == rhs.side_ && price_ == rhs.price_ &&
           created_ <= rhs.created_ && quantity_ <= rhs.quantity_;
  }
  bool operator==(const Order &rhs) const {
    return uuid_ == rhs.uuid_ && side_ == rhs.side_ && price_ == rhs.price_ &&
           quantity_ == rhs.quantity_;
  }
  friend std::ostream &operator<<(std::ostream &out, const Order &o) {
    return out << o.created_.time_since_epoch().count() << " " << o.uuid_ << " "
               << o.side_ << " " << o.price_ << " " << o.quantity_ << " "
               << o.executed_quantity_;
  }
};

/*
 * Path: /orderbook/symbol/depth
 * Example: /orderbook/ADABTC/4
 * Output:
 *    price |  amount |
 * ---------------------
 *  SELL:
 *  0.07658 | 13214.5 |
 *  0.07652 | 13554.1 |
 *  0.07646 |  5439.0 |
 *  0.07644 |   261.1 |
 *
 *  BUY:
 *  0.07638 | 15000.0 |
 *  0.07633 |  6206.4 |
 *  0.07631 |  6603.1 |
 *  0.07625 | 25000.0 |
 *
 */

class OrderQueue
    : public std::priority_queue<std::unique_ptr<Order>,
                                 std::vector<std::unique_ptr<Order>>,
                                 std::less<std::unique_ptr<Order>>> {
public:
  OrderQueue(const OrderQueue &) = delete;
  OrderQueue() = default;
  ~OrderQueue() = default;

  bool remove(std::unique_ptr<Order> &order) {
    auto it = std::find_if(c.begin(), c.end(),
                           [&](auto &&it) { return it == order; });
    if (it != c.end()) {
      c.erase(it, c.end());
      return true;
    }
    return false;
  }
  Price accumulate() const {
    return std::accumulate(c.begin(), c.end(), 0.0,
                           [](const Price amount, const auto &it) {
                             return amount + it->leftover();
                           });
  }
};

class OrderBook {
public:
  OrderBook(const OrderBook &) = delete;
  OrderBook() : buy_tree_{}, sell_tree_{} {}
  ~OrderBook() = default;

  std::map<Price, OrderQueue> buy_tree_;
  std::map<Price, OrderQueue> sell_tree_;
  void push_(std::unique_ptr<Order> &order) {
    auto &side = order->side_;
    auto &node = order->price_;
    auto &tree = side == SIDE::BUY ? buy_tree_ : sell_tree_;
    tree[node].push(std::move(order));
  }
  bool cancel(std::unique_ptr<Order> &order) {
    auto &side = order->side_;
    auto &node = order->price_;
    auto &tree = side == SIDE::BUY ? buy_tree_ : sell_tree_;
    bool result = false;
    try {
      auto &order_queue = tree.at(node);
      order->state(STATE::CANCELLED);
      result = order_queue.remove(order);
      if (order_queue.empty()) /* Drop price node */
        tree.erase(node);
      return result;
    } catch (const std::out_of_range &) { /* No price point exist */
      return result;
    }
  }

  bool match(std::unique_ptr<Order> &src) {
    auto &side = src->side_;
    auto &price = src->price_;
    auto &dist_tree = side == SIDE::BUY ? sell_tree_ : buy_tree_;
    src->state(STATE::ACTIVE);

    for (auto node = begin(dist_tree); node != end(dist_tree);) {
      auto &dist_queue = node->second;
      /* Buy cheap; sell expensive */
      if (side == SIDE::BUY ? price >= node->first : price <= node->first) {
        while (!dist_queue.empty()) {
          auto &dist = dist_queue.top();
          auto src_matching_leftover = src->leftover() - dist->leftover();
          auto dist_matching_leftover = dist->leftover() - src->leftover();
          /* Fulfilled source; partially/fulfilled dist */
          if (src_matching_leftover <= 0) {
            src->executed_quantity_ += src->leftover();
            src->state(STATE::FULFILLED);
            dist->executed_quantity_ += dist_matching_leftover;
            /* Remove from queue; delete price node if no orders leftout */
            if (dist->leftover() == 0) {
              dist->state(STATE::FULFILLED);
              dist_queue.pop();
            }
            /* Matching is complete; exit queue */
            break;
          }
          /* Partially-filled source; fulfilled dist */
          else if (src_matching_leftover > 0) {
            src->executed_quantity_ += dist->leftover();
            dist->state(STATE::FULFILLED);
            dist_queue.pop();
            /* Try next order */
            continue;
          }
        }
        /* Purge the price point with empty queue */
        if (dist_queue.empty()) {
          node = dist_tree.erase(node++);
        } else {
          ++node;
        }
        continue;
      }
      ++node;
    }

    if (src->leftover()) { /* Not enough resources to fulfill the order */
      push_(src);
      return false;
    } else /* Order's been fulfilled */
      return true;
  }

  void print_summary() const {
    auto print = [](auto &&tree) {
      for (auto &&node : tree)
        std::cout << "Price: " << node.first
                  << ", Amount: " << node.second.accumulate()
                  << ", Size: " << node.second.size() << std::endl;
    };
    std::cout << "\n\n=== Order Book Summary ===\n";
    std::cout << "----------- Buy ----------\n";
    print(buy_tree_);
    std::cout << "----------- Sell ----------\n";
    print(sell_tree_);

    std::cout << "\n\nCommission income: " << fee_income << std::endl;
  }
};
} // namespace matching_engine

int main(int argc, char **argv) {

  std::cout << "\n=============== Matching Engine ===============\n";

  matching_engine::OrderBook ob;

  // SIMULATE GEOMETRIC BROWNIAN MOTION
  double S0 = 0.04;
  double mu = 0.0;
  double sigma = 0.2;
  double T = 1;
  int steps = 999'999;
  std::vector<double> GBM = geoBrownian(S0, mu, sigma, T, steps);
  long id = 1;
  unsigned long long  total = 0;
  for (auto price : GBM) {
    auto order = std::make_unique<matching_engine::Order>();
    order->uuid_ = std::to_string(id++);
    order->price_ = ceil(price * 10000) / 10000;
    order->quantity_ = double(rand() % 5000 + 1) / (rand() % 30 + 1);
    order->side_ =
        rand() % 2 ? matching_engine::SIDE::BUY : matching_engine::SIDE::SELL;
    order->tif_ = matching_engine::TIF::GTC;
    matching_engine::TimePoint begin = matching_engine::Time::now();
    ob.match(order);
    matching_engine::TimePoint end = matching_engine::Time::now();
    total += std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
  }

  ob.print_summary();

  std::cout << "Total time spent " << total/1e+9 << std::endl;
  std::cout << "Sample size " << GBM.size() << std::endl;

  return 0;
}