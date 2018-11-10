#include "markov.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
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

static double fee_income = 0.0;
static double total_turnover = 0.0;

class Order {
public:
  Order(const std::string &uuid, const Price &price, const Price &quantity,
        const SIDE &side, const STATE &state = STATE::INACTIVE,
        const TIF &tif = TIF::GTC, const Price &executed_quantity = 0,
        const TimePoint &created = Time::now())
      : uuid_{uuid}, price_{price}, quantity_{quantity},
        executed_quantity_{executed_quantity}, side_{side}, state_{state},
        tif_{tif}, created_{created}, leftover_{quantity_ -
                                                executed_quantity_} {}
  Order() = delete;
  Order(const Order &) = delete;
  ~Order() = default;
  Price quantity() const { return quantity_; }
  Price price() const { return price_; }
  SIDE side() const { return side_; }
  void execute(const Price &quantity) {
    executed_quantity_ += quantity;
    leftover_ = quantity_ - executed_quantity_;
  }
  Price leftover() const { return leftover_; }
  TIF tif() const { return tif_; }
  void state(STATE state) {
    state_ = state;
    if (state == STATE::FULFILLED) {
      fee_income += quantity_ * 0.002;
      total_turnover += quantity_;
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
  const double quantity_;
  double executed_quantity_;
  const SIDE side_;
  STATE state_;
  TIF tif_;
  const TimePoint created_;
  double leftover_;
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
    : public std::priority_queue<std::shared_ptr<Order>,
                                 std::vector<std::shared_ptr<Order>>,
                                 std::less<std::shared_ptr<Order>>> {
public:
  OrderQueue(const OrderQueue &) = delete;
  OrderQueue() = default;
  ~OrderQueue() = default;

  bool remove(std::shared_ptr<Order> &order) {

    auto o =
        std::find_if(begin(c), end(c), [&](auto &&it) { return it == order; });
    if (o != end(c)) {
      c.erase(o, end(c));
      return true;
    }
    return false;
  }
  Price accumulate() const {
    return std::accumulate(begin(c), end(c), 0.0,
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
  void push_(std::shared_ptr<Order> &order) {
    auto &tree = order->side() == SIDE::BUY ? buy_tree_ : sell_tree_;
    tree[order->price()].push(order);
  }
  bool cancel(std::shared_ptr<Order> &order) {
    auto &tree = order->side() == SIDE::BUY ? buy_tree_ : sell_tree_;
    try {
      auto &order_queue = tree.at(order->price());
      order->state(STATE::CANCELLED);
      auto result = order_queue.remove(order);
      if (order_queue.empty()) /* Drop price node */
        tree.erase(order->price());
      return result;
    } catch (const std::out_of_range &) { /* No price point exist */
      return false;
    }
  }

  bool match(std::shared_ptr<Order> &src) {
    auto &dist_tree = src->side() == SIDE::BUY ? sell_tree_ : buy_tree_;
    src->state(STATE::ACTIVE);

    for (auto node = begin(dist_tree); node != end(dist_tree);) {
      auto &dist_queue = node->second;
      /* Buy cheap; sell expensive – conduct price improvement */
      if (src->side() == SIDE::BUY ? src->price() >= node->first
                                   : src->price() <= node->first) {
        while (!dist_queue.empty()) {
          auto &dist = dist_queue.top();
          auto matching_leftover = dist->leftover() - src->leftover();
          /* Fulfilled source; partially or fulfilled dist */
          if (matching_leftover >= 0) {
            src->execute(src->leftover());
            src->state(STATE::FULFILLED);
            dist->execute(matching_leftover == 0
                              ? dist->leftover()
                              : dist->leftover() - matching_leftover);
            /* Remove from queue; delete price node if no orders leftout */
            if (dist->leftover() == 0) {
              dist->state(STATE::FULFILLED);
              dist_queue.pop();
            }
            /* Matching is complete; exit queue */
            break;
          }
          /* Partially-filled source; fulfilled dist */
          else if (matching_leftover < 0) {
            src->execute(dist->leftover());
            dist->execute(dist->leftover());
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
    if (src->leftover() > 0) { /* Not enough resources to fulfill the order */
      push_(src);
      return false;
    } else /* Order's been fulfilled */
      return true;
  }

  Price spread() const {
    auto best_sell = begin(sell_tree_)->first;
    auto best_buy = !buy_tree_.empty() ? (--end(buy_tree_))->first : 0;

    if (!best_buy || !best_sell)
      return 0;
    return best_buy && best_sell ? (best_sell - best_buy) / best_sell : 0;
  }

  void print_summary() const {
    auto print = [](auto &&tree) {
      for (auto &&node : tree) {
        auto &&price_node = node.first;
        auto &&ob = node.second;
        std::cout << "Price: " << price_node << ", Amount: " << ob.accumulate()
                  << ", Size: " << ob.size() << std::endl;
      }
    };
    std::cout << "\n\n============ Order Book Summary ============\n";
    std::cout << "----------------- Buy ----------------\n";
    print(buy_tree_);
    std::cout << "----------------- Sell ----------------\n";
    print(sell_tree_);
    std::cout << "\nSpread: " << spread() << "\n";
    std::cout << "\nTotal turnover: " << total_turnover;
    std::cout << "\nCommission income: " << fee_income << "\n\n";
  }
};
} // namespace matching_engine

int main(int argc, char **argv) {

  std::cout.precision(8);
  std::cout << "\n=============== Matching Engine ===============\n";

  using namespace matching_engine;

  OrderBook ob;

  // SIMULATE GEOMETRIC BROWNIAN MOTION
  double S0 = 0.04;
  double mu = 0.0;
  double sigma = 0.2;
  double T = 1;
  int steps = 999'999;
  std::vector<double> GBM = geoBrownian(S0, mu, sigma, T, steps);
  long id = 1;
  std::chrono::nanoseconds elapsed;
  for (auto price : GBM) {
    auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
    auto p = ceil(price * 10'000) / 10'000;
    auto q = double(rand() % 1000 + 1) / (rand() % 20 + 1);
    auto order = std::make_shared<matching_engine::Order>(std::to_string(id++),
                                                          p, q, side);
    auto begin = std::chrono::steady_clock::now();
    ob.match(order);
    auto end = std::chrono::steady_clock::now();
    elapsed +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
  }

  ob.print_summary();

  double time = elapsed.count() / 1e+14;
  std::cout << "Total time spent: " << time << std::endl;
  std::cout << "Sample size: " << GBM.size() << std::endl;

  return 0;
}