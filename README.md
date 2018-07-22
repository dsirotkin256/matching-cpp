# Matching Service

Order-matching engine – provides access to liquidity book and coordinates live orders

## Order book

The order book is maintained (stored and continuously altered) in memory.
For faster order execution and matching the `OrderTree` (_In-Memory Order Book_) structure implementation relies on self-balancing Binary Search Tree (BST) with the nodes containing `PriceLevel` and FIFO-based Priority `OrderQueue`. For faster lookup the `GetSnapshot()` function reads LRU Cached value (`[ [side, price, depth], ...  ]`) – to avoid lazy tree traversal on each lookup; and retraverse the tree on next lookup when the cache was invalidated (so far it either when the new node/new order was Inserted or existing one was Deleted).

For rapid lookups of the market buy and market sell prices, the arrangement of price nodes in `BidOrderTree` (as the best bid price – is the most expensive buy price) will be sorted in descendant order and `AskOrderTree` in ascendant order (as the best ask – is the cheapest selling price)

```cpp
bid_price = BidOrderTree.first();
ask_price = AskOrderTree.first();
```

### Data Structure

Self-balancing Binary Search Tree (for indexing bids and asks)
|
|
 ---> Price Node
                |
                 ---> Price
                |
                 ---> Orders Priority Queue (time, quantity)


Functions:

- `ME::OB::GetSnapshot()` – Lookup tree snapshot`[ [side, price, depth], ...  ]`
- `ME::OB::GetMarketBidPrice()`
- `ME::OB::GetMarketAskPrice()`
- `ME::OB::LookupPriceNode(price level)`
- `ME::OB::Match(o)` – Try to match order else push into the price node queue
- `ME::OB::Cancel(o)` – Attempt to cancel the order from the order book
- `ME::OB::Execute_(o)` – Attempt to execute the order given the conditional scenario 
- `ME::OB::RebuildOrderTree_()` – Rebuild order book on startup
- `ME::OB::InsertOrder_()`
- `ME::OB::RemoveOrder_()`
- `ME::Cache::InvalidateCache(key)`
- `ME::Cache::SetCacheValue(key)`
- `ME::Cache::GetCacheValue(key)` – Avoid lazy tree traversal on each lookup
- `ME::DB::SelectOrders(side)` – Retreive bid/ask orders from database
- `ME::DB::InsertOrder()` – Populate order in database
- `ME::DB::CancelOrder()` – Update order state to `cancelled`
- `ME::DB::MatchOrders_(o1, o2)` – Update orders details (executed amount, status, state, handle audit records)

Insert time: O(1)
Lookup worst-case scenario: O(log(n)) – where `n` is a number of orders in the order book


## Order structure and execution behavior

### Order sides
- BID (Buy)
- ASK (Sell)

### Transition states

- RECEIVED – An order is received by Order Dispatcher
- ACTIVE – An order is resting on the order book and waiting for execution
- DONE – Orders which are no longer resting on the order book (see status)
- SETTLED – An order is settled when all of the fills have settled and the remaining holds

### Statuses

- NEW
- PARTIALLY_FILLED
- FILLED
- CANCELED
- REJECTED
- EXPIRED

### Order types

- LIMIT
- MARKET
- STOP_LOSS
- STOP_LOSS_LIMIT
- TAKE_PROFIT
- TAKE_PROFIT_LIMIT
- LIMIT_MAKER

### Time in force constraints
- Good for day order (GFD)
- Good-til-cancelled (GTC)
- Immediate or cancel (IOC)
- Fill or Kill (FOC)

## Historical prices

## Architecture

### Network layer

The server-to-server communication is reachable via RPC calls

#### Streaming

#### RPC

### Persistence layer

The service uses **Transaction database (TDB)** to store trading transactions and maintain accounts balances and **Chronical Database (CDB)** to update *historical prices*. 

### Queuing layer

The client system pushes serialised order object as a message in 

The implementation is on top of Message Queueing system where API server publishes an order to the corresponding topic and the fastest matching daemon who receives the order first tries to execute it.

#### Use case
Client wants to buy/sell/cancel `BTC_ETH` (e.g. buy amount of BTC at price of ETH <- matches -> buy amount of ETH at price of BTC) hence they make a request to `/(buy|sell|cancel)` -> Matching engine dæmons receive the message, fetch open orders from the order book and executes the orders which satisfy matching conditions -> Returns response to the client on order status. Note, clients may subscribe and listen to orders updates or order book updates. In this scenario, the Pub/Sub's cannels come in to serve the purpose to notify the recepints about updates.

#### Questions we should consider

1.  Durability - messages may be kept in memory, written to disk, or even committed to a DBMS if the need for reliability indicates a more resource-intensive solution. (We don't need order persistence before actual processing by the matching dæmons because it will impact on performance and won't bring any value for us – so "let it fail")

2. Security policies - which applications should have access to these messages?

3. Message purging policies - queues or messages may have a "time to live" (if the orders weren't processed by the matching engine within 10 seconds they cancelled automatically)

4. Message filtering - some systems support filtering data so that a subscriber may only see messages matching some pre-specified criteria of interest. (those will be separated by market pairs `BTC_ETH`, `LTC_XRP`, `BTC_CFEX`)

5. Delivery policies - do we need to guarantee that a message is delivered at least once, or no more than once? (Yes, the order must be processed **only once**, no a single opportunity for race conditions – so by having this we can guarantee a horizontal scaling of our matching workers)

6. Routing policies - in a system with many queue servers, what servers should receive a message or a queue's messages?

7. Batching policies - should message be delivered immediately? Or should the system wait a bit and try to deliver many messages at once? (Must be consumed one-by-one to avoid price slippage)

8. Queuing criteria - when should a message be considered "enqueued"? When one queue has it? Or when it has been forwarded to at least one remote queue? Or to all queues?

9. Receipt notification - A publisher may need to know when some or all subscribers have received a message (Subscriber doesn't have to know about it but the response that the order has been forwarded further to matching engine processing step is desirable, or a status update).

10. What if order execution failed after the order had been removed from the queue? (Let it fail)

#### Test scope
- message consumed only once or cancelled
- an order is executed once or failed
- orders are fulfilled according to execution type and availability of opposite orders in the market
- orders are persisted in data storage after execution
- account balances are correctly updated and consistent with the trading history

#### Redis
Matching dæmons are initiated and ready to consume and match the orders (using `SUBSCRIBE` command [3])
API pushes the order to the head of Redis `buy`/`sell`/`cancel` queue (using `LPUSH` command [1])
Then the API publishes the notification (using `PUBLISH` command [2]) to PubSub channel, so the matching dæmons know when to read the queue (via `SUBSCRIBE`). Then dæmons compete for an order the one who consumes (using `BRPOP` command [4] by fetching and then removing the order from the tail of the queue) the order from the queue first will process it. The orders are consumed according to the queue priority (1st – buy/sell, 2nd – cancel). In addition, we must guarantee that if the order was cancelled then buy/sell request cannot be executed to fulfil the order.


#### References
[1] https://redis.io/commands/lpush

[2] https://redis.io/commands/publish

[3] https://redis.io/commands/subscribe

[4] https://redis.io/commands/brpop

[5] https://redis.io/topics/sentinel

[6] https://martinfowler.com/articles/lmax.html

