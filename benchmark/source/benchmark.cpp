#include "fair_value.hpp"
#include "orderbook.hpp"
#include "types.hpp"

#include <benchmark/benchmark.h>

namespace {

constexpr int kMidPrice = 50;
constexpr kalshi::Quantity kLevelQuantity =
    kalshi::Quantity::from_contracts(100);
constexpr int kAltPriceA = 49;
constexpr int kAltPriceB = 48;
constexpr kalshi::Quantity kDeltaQuantity =
    kalshi::Quantity::from_contracts(200);
constexpr int kSmallLevelCount = 10;
constexpr int kMediumLevelCount = 50;
constexpr int kLargeLevelCount = 100;
constexpr double kMidCents = 50.0;
constexpr double kHighMidCents = 65.0;
constexpr double kShortTimeHours = 0.5;
constexpr double kMediumTimeHours = 2.0;
constexpr int kLargeInventory = 40;

kalshi::Orderbook make_snapshot(int level_count) {
  kalshi::Orderbook snap;
  snap.ticker = "BENCH-TICKER";
  for (int index = 0; index < level_count; ++index) {
    snap.yes.push_back(
        {.price_cents = kMidPrice - index, .quantity = kLevelQuantity});
    snap.no.push_back(
        {.price_cents = kMidPrice - index, .quantity = kLevelQuantity});
  }
  return snap;
}

} // namespace

// ---- LocalOrderbook benchmarks ----

static void BM_ApplySnapshot(benchmark::State &state) {
  const auto snap = make_snapshot(static_cast<int>(state.range(0)));
  for ([[maybe_unused]] auto iter : state) {
    kalshi::LocalOrderbook book;
    book.apply_snapshot(snap);
    benchmark::DoNotOptimize(book);
  }
}
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
BENCHMARK(BM_ApplySnapshot)
    ->Arg(kSmallLevelCount)
    ->Arg(kMediumLevelCount)
    ->Arg(kLargeLevelCount);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

static void BM_ApplyDelta(benchmark::State &state) {
  const auto snap = make_snapshot(static_cast<int>(state.range(0)));
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  int price = kAltPriceA;
  for ([[maybe_unused]] auto iter : state) {
    book.apply_delta(kalshi::Side::Yes, price, kDeltaQuantity);
    price = (price == kAltPriceA) ? kAltPriceB : kAltPriceA;
    benchmark::DoNotOptimize(book);
  }
}
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
BENCHMARK(BM_ApplyDelta)
    ->Arg(kSmallLevelCount)
    ->Arg(kMediumLevelCount)
    ->Arg(kLargeLevelCount);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

static void BM_BestBid(benchmark::State &state) {
  kalshi::LocalOrderbook book;
  book.apply_snapshot(make_snapshot(static_cast<int>(state.range(0))));
  for ([[maybe_unused]] auto iter : state) {
    auto bid = book.best_bid();
    benchmark::DoNotOptimize(bid);
  }
}
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
BENCHMARK(BM_BestBid)->Arg(kSmallLevelCount)->Arg(kLargeLevelCount);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// ---- FairValueEngine benchmarks ----

static void BM_FairValueEstimate(benchmark::State &state) {
  kalshi::FairValueEngine engine{std::make_unique<kalshi::HeuristicModel>()};
  const kalshi::FairValueInput input{.mid_cents = kMidCents,
                                     .time_to_close_hours = kMediumTimeHours,
                                     .net_position = 0,
                                     .external_prob = std::nullopt};
  for ([[maybe_unused]] auto iter : state) {
    double fair_value = engine.estimate(input);
    benchmark::DoNotOptimize(fair_value);
  }
}
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
BENCHMARK(BM_FairValueEstimate);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

static void BM_FairValueEstimateWithInventory(benchmark::State &state) {
  kalshi::FairValueEngine engine{std::make_unique<kalshi::HeuristicModel>()};
  const kalshi::FairValueInput input{.mid_cents = kHighMidCents,
                                     .time_to_close_hours = kShortTimeHours,
                                     .net_position = kLargeInventory,
                                     .external_prob = std::nullopt};
  for ([[maybe_unused]] auto iter : state) {
    double fair_value = engine.estimate(input);
    benchmark::DoNotOptimize(fair_value);
  }
}
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
BENCHMARK(BM_FairValueEstimateWithInventory);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,modernize-avoid-c-arrays)
BENCHMARK_MAIN();
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,modernize-avoid-c-arrays)
