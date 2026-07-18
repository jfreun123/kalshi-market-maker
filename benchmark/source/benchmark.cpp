#include "core/types.hpp"
#include "engine/fair_value.hpp"
#include "engine/orderbook.hpp"
#include "engine/risk_manager.hpp"
#include "exchange/order_manager.hpp"
#include "strategy/quoter.hpp"

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

// ---- Quoter benchmarks ----

namespace {

// In-memory IOrderManager: no HTTP, deterministic ids — isolates the Quoter's
// pricing-path cost from transport.
class StubOrderManager final : public kalshi::IOrderManager {
public:
  kalshi::Order place(std::string_view ticker, kalshi::Side side,
                      int price_cents, int quantity) override {
    kalshi::Order order;
    order.id = "stub-" + std::to_string(next_id_++);
    order.market_ticker = std::string{ticker};
    order.side = side;
    order.price_cents = price_cents;
    order.quantity = kalshi::Quantity::from_contracts(quantity);
    open_orders_[order.id] = order;
    return order;
  }
  bool cancel(std::string_view order_id) override {
    return open_orders_.erase(std::string{order_id}) > 0;
  }
  void cancel_all(std::string_view) override { open_orders_.clear(); }
  std::optional<std::string> amend(std::string_view order_id, std::string_view,
                                   kalshi::Side, int,
                                   kalshi::Quantity) override {
    return std::string{order_id};
  }
  bool record_fill(const kalshi::Fill &) override { return true; }
  [[nodiscard]] kalshi::Quantity net_position(std::string_view) const override {
    return {};
  }
  [[nodiscard]] double realized_pnl(std::string_view) const override {
    return 0.0;
  }
  [[nodiscard]] double fees_paid(std::string_view) const override {
    return 0.0;
  }
  [[nodiscard]] double unrealized_pnl(std::string_view, int) const override {
    return 0.0;
  }
  [[nodiscard]] double position_cost(std::string_view) const override {
    return 0.0;
  }
  [[nodiscard]] kalshi::ExposureDecomposition
  exposure_decomposition(std::string_view) const override {
    return {};
  }
  [[nodiscard]] const std::unordered_map<std::string, kalshi::Order> &
  open_orders() const override {
    return open_orders_;
  }

private:
  std::unordered_map<std::string, kalshi::Order> open_orders_;
  int next_id_{0};
};

} // namespace

// Steady-state per-delta cost: quotes rest, desired price is unchanged, the
// quoter prices the book (minus its own quotes) and decides to do nothing.
// This is the path taken on nearly every book update in production.
static void BM_QuoterUpdateSteadyState(benchmark::State &state) {
  StubOrderManager order_mgr;
  kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
  kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

  const auto snap = make_snapshot(static_cast<int>(state.range(0)));
  kalshi::LocalOrderbook book;
  book.apply_snapshot(snap);
  quoter.update(snap.ticker, book);

  for ([[maybe_unused]] auto iter : state) {
    quoter.update(snap.ticker, book);
    benchmark::DoNotOptimize(quoter);
  }
}
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
BENCHMARK(BM_QuoterUpdateSteadyState)
    ->Arg(kSmallLevelCount)
    ->Arg(kMediumLevelCount)
    ->Arg(kLargeLevelCount);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,modernize-avoid-c-arrays)
BENCHMARK_MAIN();
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory,cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,modernize-avoid-c-arrays)
