# Setup

## Rebuild the project
```bash
./scripts/build.sh
```

## Start the engine
```bash
./build/bin/matcher
```

## Debugging
```bash
gdb --tui ./build/bin/matcher
```

# Docker setup

## Rebuild and tag a new image

```bash
docker build -t nrdwnd/exchange:latest .
```

## Run the image in a container

Create a fresh container and run in interactive mode:
```bash
docker run -it nrdwnd/exchange:latest
```

Enabled security features for hacking/debugging:
```bash
docker run -it --privileged -v ${PWD}:/opt/matching --cap-add=SYS_PTRACE --security-opt seccomp=unconfined nrdwnd/exchange:latest
```

Start the existing container in interactive mode:
```bash
docker container start -i <CONTAINER_ID>
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
