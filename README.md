# Install

```bash
./build.sh
```

# Run

```bash
./build/bin/matcher
```

# Debug

```bash
lldb ./build/bin/matcher
```

# Matching Service
Order-matching engine – provides access to liquidity book and coordinates live orders

Matching engine provides guarantees that the orders are sorted by price and time.
For buy (bid) side, orders are sorted in descending order of price. Order with highest bid price is first and lowest bid price is last. If price is same then ordering is by time. The order that arrived earlier gets preference.
For sell (ask) side, orders are sorted in ascending order of price. Order with lowest ask price is first and highest ask price is last. If price is same then ordering is by time. The order that arrived earlier gets preference.
We have done exhaustive testing of our matching engine to ensure that fairness is guaranteed.

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
bid_price = BidOrderTree.first();
ask_price = AskOrderTree.first();
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

### Networking
The server-to-server communication is reachable via RPC (HTTP/2) calls via
- **gRPC** – Remote Procedure Call (RPC) Framework which allows bidirectional
streaming and flow control, blocking or nonblocking bindings, and cancellation and timeouts
- and **Protocol Buffers** (Interface Definition Language) for serialising 
structured data and service definitions

### Storage
The service uses **Transaction database** (TDB) to store trading transactions 
and maintain accounts balances and **Chronical Database** (CDB) to update *historical prices*.
