#include "OrderBook.h"

using namespace matching_engine;

Order::Order(const std::string &uuid, const Price &price, const Price &quantity,
    const SIDE &side, const STATE &state,
    const TIF &tif, const Price &executed_quantity,
    const TimePoint &created) : uuid_{uuid}, price_{price}, quantity_{quantity},
  executed_quantity_{executed_quantity}, side_{side}, state_{state}, 
  tif_{tif}, created_{created}, leftover_{quantity_ - executed_quantity_} { }

Price Order::quantity() const { return quantity_; }

Price Order::price() const { return price_; }
SIDE Order::side() const { return side_; }
void Order::execute(const Price &quantity) {
  executed_quantity_ += quantity;
  leftover_ = quantity_ - executed_quantity_;
}
Price Order::leftover() const { return leftover_; }
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

bool OrderQueue::remove(std::shared_ptr<Order> &order) {
  auto o =
      std::find_if(begin(c), end(c), [&](auto &&it) { return it == order; });
  if (o != end(c)) {
    c.erase(o, end(c));
    return true;
  }
  return false;
}
Price OrderQueue::accumulate() const {
  return std::accumulate(begin(c), end(c), 0.0,
                         [](const Price amount, const auto &it) {
                           return amount + it->leftover();
                         });
}

OrderBook::OrderBook() : buy_tree_{OrderBook::Comp{OrderBook::Comp::greater}}, sell_tree_{OrderBook::Comp{OrderBook::Comp::less}} {}
OrderBook::OrderBook(const std::shared_ptr<spdlog::logger> &logger)
    : logger_{logger}, buy_tree_{OrderBook::Comp{OrderBook::Comp::greater}}, sell_tree_{OrderBook::Comp{
      OrderBook::Comp::less}} {}

bool OrderBook::cancel(std::shared_ptr<Order> &order) {
  std::unique_lock<std::shared_mutex> l{m_};
  auto &tree = order->is_buy() ? buy_tree_ : sell_tree_;
  try {
    auto &&order_queue = tree.at(order->price());
    auto result = order_queue.remove(order);
    order->state(STATE::CANCELLED);
    if (order_queue.empty()) /* Drop price node */
      tree.erase(order->price());
    return result;
  } catch (const std::out_of_range &) { /* No price point exist */
    return false;
  }
}

bool OrderBook::match(std::shared_ptr<Order> &src) {
  auto &src_tree = src->is_buy() ? buy_tree_ : sell_tree_;
  auto &dist_tree = src->is_buy() ? sell_tree_ : buy_tree_;
  src->state(STATE::ACTIVE);

  std::unique_lock<std::shared_mutex> l{m_};
  for (auto [node, exit_tree] = std::tuple{begin(dist_tree), false};
       !exit_tree && node != end(dist_tree);) {
    auto &&dist_queue = node->second;
    /* Buy cheap; sell expensive – conduct price improvement */
    if (src->is_buy() ? src->price() >= node->first
                      : src->price() <= node->first) {
      bool exit_queue = false;
      while (!exit_queue && !dist_queue.empty()) {
        auto &dist = dist_queue.top();
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
            dist_queue.pop();
          }

          /* Matching is complete */
          exit_queue = true;
          exit_tree = true;
        }
        /* Partially-filled source; fulfilled dist */
        else {
          src->execute(dist->leftover());
          dist->execute(dist->leftover());
          dist->state(STATE::FULFILLED);
          dist_queue.pop();
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
      exit_tree = true;
    }
  }
  /* Not enough resources to fulfill the order; push to source tree */
  if (src->leftover() > 0) {
    const auto &found = src_tree.find(src->price());
    if (found == src_tree.end()) { /* Create new price node */
      const auto &inserted = src_tree.insert(
          found,
          std::move(std::make_pair(src->price(), std::move(OrderQueue()))));
      inserted->second.push(src);
    } else { /* Insert in existing price node */
      found->second.push(src);
    }
    return false;
  }
  /* Order's been fulfilled */
  return true;
}

Price OrderBook::best_buy() const {
  std::shared_lock<std::shared_mutex> l{m_};
  return !buy_tree_.empty()
             ? begin(buy_tree_)->first
             : !sell_tree_.empty() ? begin(sell_tree_)->first : 0;
}

Price OrderBook::best_sell() const {
  std::shared_lock<std::shared_mutex> l{m_};
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
  std::shared_lock<std::shared_mutex> l{m_};
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
      if (limit >= 10)
        break;
    }
  };
  traverse(buy_tree_, SIDE::BUY);
  traverse(sell_tree_, SIDE::SELL);
  return snapshot;
}
