# Setup

### Build

```bash
./build.sh
```

### Run

```bash
./build/bin/matcher
```

### Debug

```bash
lldb ./build/bin/matcher
```

# Docker setup

### Build

```bash
docker build .
```

### Run

```bash
docker run -it -v ${PWD}:/opt/matching --cap-add=SYS_PTRACE --security-opt seccomp=unconfined matching:latest /bin/bash
```

### Debug

```bash
lldb-5.0 ./build/bin/matcher
```

# Matching Service
Order-matching engine — provides access to liquidity book and coordinates live orders

Matching engine provides guarantees that the orders are sorted by price and time.
For buy (bid) side, orders are sorted in descending order of price. Order with highest bid price is first and lowest bid price is last. If price is same then ordering is by time. The order that arrived earlier gets preference.
For sell (ask) side, orders are sorted in ascending order of price. Order with lowest ask price is first and highest ask price is last. If price is same then ordering is by time. The order that arrived earlier gets preference.
We have done exhaustive testing of our matching engine to ensure that fairness is guaranteed.

- Below is an example of [Geometric Brownian Motion](https://en.wikipedia.org/wiki/Geometric_Brownian_motion) simulation with 1m samples and `σ = 0.01`

```
============ Order Book ============

-----------------------------------
|        |             |          |
|  Price |    Volume   |    Size  |
|        |             |          |
---------------- Buy --------------
|0.0397  |     44113.18|       519|
|0.0395  |     66039.30|       733|
|0.0394  |     40262.44|       480|
|--------------- Sell ------------|
|0.0398  |       427.56|         7|
|0.0399  |     80162.80|       825|
|0.0400  |      3550.73|        34|
|0.0401  |      4371.03|        40|
-----------------------------------


========== Trading summary =========

-----------------------------------
|Buy volume      |       150414.92|
|Sell volume     |        88512.11|
|Buy total       |         5946.19|
|Sell total      |         3532.82|
|Spread          |           0.25%|
|Turnover        |     90975371.60|
|Commission      |       181950.74|
-----------------------------------


========== Sample Summary =========

Time elapsed: 0.922858 sec.
Lookup time: 0.00025 sec.
Sample size: 1000000
```

- Stress test with 1m GBM samples every 5s — 2m active orders after 3h uptime fit in 300MB memory space

[![asciicast](https://asciinema.org/a/N3TFtzxOXGuOL1emF29tmhiwM.svg)](https://asciinema.org/a/N3TFtzxOXGuOL1emF29tmhiwM)

## Service-level API
- `GetOrderBook(): OrderBook` – Order book snapshot `[ [side, price, depth], ...  ]`.
- `GetBestBidPrice(): Number` – The most expensive buying price available at which an asset might be sold out on the market.
- `GetBestBidPrice(): Number` – The cheapest selling price available at which an asset might be purchased on the market.
- `GetSpread(): Number` – Returns percent market spread `(b.ask - b.bid) / b.ask * 100`.
- `GetQuote(): Number` – Returns the most recent price on which a buyer and seller agreed and at which some amount of the asset was transacted.
- `OrderLookup(OrderID): Order` – Returns active order details if the order sits in the order book.
- `Buy(Order): Status` – Submits buy order.
- `Sell(Order): Status` – Submits sell order.
- `Cancel(OrderID): Status` – Submits order cancel request.

## Architecture Layers
### Computing

The order book is maintained (stored and continuously altered) in memory.
For faster order execution and matching the `OrderTree` (*In-Memory Order Book*)
structure implementation is based on self-balancing Binary Search Tree (BST)
with the nodes containing `PriceLevel` and FIFO-based Priority `OrderQueue`.
For faster lookup the `GetSnapshot()` function reads LRU Cached value (`[ [side, price, depth], ...  ]`) – to avoid
lazy tree traversal on each lookup; and retraverse the tree on next lookup when
the cache was invalidated (so far it either when the new node/new order was Inserted or existing one was Deleted).

For rapid lookups of the market buy and market sell prices, the arrangement of
price nodes in `BidOrderTree` (as the best bid price – is the most expensive buy price)
will be sorted in descendant order and `AskOrderTree` in ascendant
order (as the best ask – is the cheapest selling price):

```cpp
class OrderBook {
    struct Comp {
        enum compare_type { less, greater };
        explicit Comp(compare_type t) : type(t) {}
        template <class T, class U> bool operator()(const T &t, const U &u) const {
        return type == less ? t < u : t > u;
     }
     compare_type type;
   };

public:
    OrderBook(const OrderBook &) = delete;
    OrderBook() : buy_tree_{Comp{Comp::greater}}, sell_tree_{Comp{Comp::less}} {}
    std::map<Price, OrderQueue, Comp> buy_tree_;
    std::map<Price, OrderQueue, Comp> sell_tree_;
  ...  
};

...
bid_price = orderbook.buy_tree().first();
ask_price = orderbook.sell_tree().first();
...
orderbook.match(limit_buy_order);
orderbook.match(limit_sell_order);
...
orderbook.cancel(limit_buy_order);
orderbook.cancel(limit_sell_order);
...
```

Order book uses self-balancing Binary Search Tree (represents order tree) to
maintain active orders in memory:
<pre>
                          +-----------+
                          |           |
               +----------+ PriceNode +----------+
               |          |           |          |
               |          +-----------+          |
         +-----v-----+                     +-----v-----+
         |           |                     |           |
      +--+ PriceNode +--+               +--+ PriceNode +--+
      |  |           |  |               |  |           |  |
      |  +-----------+  |               |  +-----------+  |
      |                 |               |                 |
+-----v-----+     +-----v-----+   +-----v-----+     +-----v-----+
|           |     |           |   |           |     |           |
| PriceNode |     | PriceNode |   | PriceNode |     | PriceNode |
|           |     |           |   |           |     |           |
+-----------+     +-----------+   +-----------+     +-----------+
</pre>

**Order book structure:**
- Market
- Ask Order Tree
- Bid Order Tree

Price node consists of *price value* and *priotity queue* of orders of same price level :
<pre>
+---------------+
|     Price     |
+---------------+
|  Order queue  |
| +--+--+--+--+ |
| |  |  |  |  | |
| +-----------+ |
+---------------+
</pre>

**Order structure:**
- Market
- Price
- Size
- Executed amount
- Author
- Expiration timestamp
- Creation timestamp
- Last update timestamp
- Order side
    - Bid – Buy
    - Ask – Sell
- Transition State
    - RECEIVED – An order is received by Order Dispatcher
    - ACTIVE – An order is resting on the order book and waiting for execution
    - DONE – Orders which are no longer resting on the order book (see status)
    - SETTLED – An order is settled when all of the fills have settled and the remaining holds
- Status
    - NEW
    - PARTIALLY_FILLED
    - FILLED
    - CANCELED
    - REJECTED
    - EXPIRED
- Type
    - LIMIT
    - MARKET
    - STOP_LOSS
    - STOP_LOSS_LIMIT
    - TAKE_PROFIT
    - TAKE_PROFIT_LIMIT
    - LIMIT_MAKER
- Time in force code
    - GFD – Good for day order
    - GTC – Good-til-cancelled
    - IOC – Immediate or cancel
    - FOC – Fill or Kill

**Functions and behaviour:**
- `ME::OB::GetSnapshot()` – Lookup tree snapshot`[ [side, price, depth], ...  ]`
- `ME::OB::GetMarketBidPrice()`
- `ME::OB::GetMarketAskPrice()`
- `ME::OB::LookupPriceNode(price level)`
- `ME::OB::Cancel(o)` – Attempt to cancel the order from the order book
- `ME::OB::Execute(o)` – Attempt to match order given the conditional scenario else push into the price node queue
- `ME::OB::RebuildOrderBook()` – Rebuild order book on startup
- `ME::DB::FetchActiveOrders(market)` – Retreive bid/ask orders from database
- `ME::DB::CancelOrder()` – Update order state to `cancelled`
- `ME::DB::MatchOrders_(o1, o2)` – Update orders details (executed amount, status, state, handle audit records)


### Storage
The service uses **Transaction database** (TDB) to store trading transactions 
and maintain accounts balances and **Chronical Database** (CDB) to update *historical prices*.
