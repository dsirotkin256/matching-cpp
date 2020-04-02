#include <benchmark/benchmark.h>
#include <vector>
#include <orderbook.hpp>
#include <order_router.hpp>
#include <tcp_server.hpp>
#include "markov.h"

using namespace matching_engine;

static auto SimulateMarket(int count) {
  double S0 = 80;
  double mu = double(rand() % 5 + 1) / 100 * (rand() % 2 ? 1 : -1); // add rand drift
  double sigma = 0.08 + double(rand() % 2 + 1) / 1000; // flat vol + rand
  double T = 1;
  return geoBrownian(S0, mu, sigma, T, count);
}

static void OrderCreation(benchmark::State& state) {
  auto market = "USD_JPY";
  auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
  Price price = rand() % 100 + 1;
  Quantity quantity = double(rand() % 100 + 1) / (rand() % 20 + 1);
  for (auto _ : state) {
    auto order = std::make_unique<Order>(market, side, price, quantity);
  }
}
BENCHMARK(OrderCreation)->DenseRange(0, 1000, 250);

static void OrderMatching(benchmark::State& state) {
  // Perform setup here
  std::string market = "USD_JPY";
  OrderBook ob(market);
  auto prices = SimulateMarket(state.range(0));
  for(auto _ : state) {
    for (auto price : prices) {
      auto side = rand() % 2 ? SIDE::BUY : SIDE::SELL;
      auto quantity = double(rand() % 10 + 1) / (rand() % 20 + 1);
      auto order = std::make_unique<Order>(market, side, price, quantity);
      //dispatcher->send(std::move(order));
      ob.match(std::move(order));
    }
  }
}
BENCHMARK(OrderMatching)->DenseRange(0, 1000000, 100000);
// Run the benchmark
BENCHMARK_MAIN();
