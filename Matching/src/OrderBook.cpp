#include "OrderBook.h"

using namespace matching_engine;

Order::Order(const std::string& market_name,
    const UUID &uuid, const Price &price, 
    const double &quantity,const SIDE &side, const STATE &state,
    const TIF &tif, const double &executed_quantity,
    const TimePoint &created) : market_name_{market_name}, uuid_{uuid}, price_{price}, quantity_{quantity},
  executed_quantity_{executed_quantity}, side_{side}, state_{state}, 
  tif_{tif}, created_{created}, leftover_{quantity_ - executed_quantity_} { }

std::string Order::market_name() const { return market_name_; }
double Order::quantity() const { return quantity_; }
UUID Order::uuid() const { return uuid_; }
Price Order::price() const { return price_; }
SIDE Order::side() const { return side_; }
void Order::execute(const double &quantity) {
  executed_quantity_ += quantity;
  leftover_ = quantity_ - executed_quantity_;
}
double Order::leftover() const { return leftover_; }
TIF Order::tif() const { return tif_; }
void Order::state(STATE state) {
  state_ = state;
}
bool Order::is_buy() const { return side_ == SIDE::BUY; }

bool Order::operator>=(const Order &rhs) const {
  return side_ == rhs.side_ && price_ == rhs.price_ &&
    created_ <= rhs.created_ && quantity_ >= rhs.quantity_;
}
bool Order::operator==(const Order &rhs) const {
  return uuid_ == rhs.uuid_ && side_ == rhs.side_ && price_ == rhs.price_ &&
    quantity_ == rhs.quantity_;
}

template <typename order_type>
OrderQueue::OrderQueue(order_type order) {
  emplace_back(std::move(order));
}

template <typename order_type, typename... args>
OrderQueue::OrderQueue(order_type order, args... rest) {
  emplace_back(std::move(order));
  OrderQueue(rest...);
}

bool OrderQueue::remove(const UUID uuid) {
  auto order =
    std::find_if(cbegin(), cend(), [&](const auto& it) { return it->uuid() == uuid; });
  if (order != cend()) {
    erase(order, cend());
    return true;
  }
  return false;
}

Price OrderQueue::accumulate() const {
  return tbb::parallel_reduce(tbb::blocked_range<OrderQueue::const_iterator>(cbegin(),cend()), 0.0, [](const tbb::blocked_range<OrderQueue::const_iterator> r, double total) {
      return std::accumulate(r.begin(), r.end(), total,
          [](const double subtotal, const auto& order) {
          return subtotal + order->leftover();
          });
      }, std::plus<double>());
}

OrderBook::OrderBook() : buy_tree_{OrderBook::Comp{OrderBook::Comp::greater}}, sell_tree_{OrderBook::Comp{OrderBook::Comp::less}} {}
OrderBook::OrderBook(const std::string market_name)
  : buy_tree_{OrderBook::Comp{OrderBook::Comp::greater}},
  sell_tree_{OrderBook::Comp{OrderBook::Comp::less}},
  market_name_{market_name} {}

bool OrderBook::cancel(const UUID uuid, const SIDE side, const Price price) {
  auto &tree = side == SIDE::BUY ? buy_tree_ : sell_tree_;
  try {
    auto &&order_queue = tree.at(price);
    auto result = order_queue.remove(uuid);
    if (order_queue.empty()) /* Drop price node */
      tree.erase(price);
    return result;
  } catch (const std::out_of_range &) { /* No price point exist */
    return false;
  }
}

std::string OrderBook::market_name() const {
  return market_name_;
}

bool OrderBook::match(OrderPtr src) {
  auto &src_tree = src->is_buy() ? buy_tree_ : sell_tree_;
  auto &dist_tree = src->is_buy() ? sell_tree_ : buy_tree_;
  src->state(STATE::ACTIVE);

  auto should_exit_tree = false;
  auto node = begin(dist_tree);
  while (!should_exit_tree && node != end(dist_tree)) {
    auto &&dist_queue = node->second;
    /* Buy cheap; sell expensive – conduct price improvement */
    if (src->is_buy() ? src->price() >= node->first
        : src->price() <= node->first) {
      bool exit_queue = false;
      while (!exit_queue && !dist_queue.empty()) {
        auto dist = dist_queue.front().get();
        auto leftover = dist->leftover() - src->leftover();

        /* Fulfilled source; partially or fulfilled dist */
        if (leftover >= 0) {
          src->execute(src->leftover());
          src->state(STATE::FULFILLED);

          if (leftover == 0) { /* Exact match */
            dist->execute(dist->leftover());
          } else { /* Partial match */
            dist->execute(dist->leftover() - leftover);
          }
          /* Remove fulfilled order from queue */
          if (dist->leftover() == 0) {
            dist->state(STATE::FULFILLED);
            dist_queue.pop_front();
          }

          /* Matching is complete */
          exit_queue = true;
          should_exit_tree = true;
        }
        /* Partially-filled source; fulfilled dist */
        else {
          src->execute(dist->leftover());
          dist->execute(dist->leftover());
          dist->state(STATE::FULFILLED);
          /* Remove fulfilled order from queue */
          dist_queue.pop_front();
          /* Try next order in the queue */
        }
      }
      /* Try next price node */
      if (dist_queue.empty()) { /* Purge the price point with empty queue */
        node = dist_tree.erase(node++);
      } else {
        ++node;
      }
    } else {
      should_exit_tree = true;
    }
  }
  /* Not enough resources to fulfill the order; push to source tree */
  if (src->leftover() > 0) {
    const auto &node = src_tree.find(src->price());
    if (node == src_tree.end()) { /* Create new price node */
      src_tree.emplace_hint(
          node,
          src->price(),
          std::move(src));
    } else { /* Insert in existing price node */
      node->second.emplace_back(std::move(src));
    }
    return false;
  }
  /* Order's been fulfilled */
  return true;
}

Price OrderBook::best_buy() const {
  return !buy_tree_.empty()
    ? begin(buy_tree_)->first
    : !sell_tree_.empty() ? begin(sell_tree_)->first : 0;
}

Price OrderBook::best_sell() const {
  return !sell_tree_.empty()
    ? begin(sell_tree_)->first
    : !buy_tree_.empty() ? begin(buy_tree_)->first : 0;
}

Price OrderBook::quote() const { return (best_buy() + best_sell()) / 2; }

Price OrderBook::spread() const {
  auto buy = best_buy();
  auto sell = best_sell();
  return buy && sell ? (sell - buy) / sell : 0;
}

std::vector<OrderBook::snapshot_point> OrderBook::snapshot() const {
  std::vector<snapshot_point> snapshot;
  auto traverse = [&](const auto &tree, const SIDE &side) {
    auto limit = 0;
    for (auto &&node : tree) {
      snapshot_point point;
      point.side = side;
      point.price = node.first;
      point.cumulative_quantity = node.second.accumulate();
      point.size = node.second.size();
      snapshot.emplace_back(point);
      limit++;
      if (limit >= 20)
        break;
    }
  };
  traverse(buy_tree_, SIDE::BUY);
  traverse(sell_tree_, SIDE::SELL);
  return snapshot;
}
