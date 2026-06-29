# Kalshi Market Maker — Build Plan

## Architecture

```mermaid
graph TD
    subgraph Exchange
        REST["REST API"]
        WS["WebSocket Feed"]
    end
    subgraph Core
        AUTH["Auth (RSA-PSS-SHA256)"]
        HTTP["RestClient"]
        WSCONN["WebSocketClient"]
        SESS["TradingSession (engine)"]
        OB["LocalOrderbook"]
        OM["OrderManager"]
        RM["RiskManager + global kill-switch"]
        FV["FairValueEngine"]
        QE["Quoter"]
        PF["Portfolio (read-model)"]
    end
    REST <-->|signed| AUTH --> HTTP
    WS -->|snapshot/delta/fill| WSCONN --> SESS
    SESS -->|on_delta| OB --> FV --> QE
    SESS -->|on_fill| OM --> HTTP
    RM --> QE --> OM
    OM --> RM
    OM --> PF --> RM
```

`main.cpp` does process/IO only (config, logger, signals, transports, WS thread,
PnL persistence). `TradingSession` (`source/trading_session.hpp/.cpp`) owns the
domain reactions (snapshot/delta/fill, portfolio kill-switch, status logging) so
the same wiring runs in production, unit tests, and session replay. Capture (raw
WS frames + REST responses) is teed by the `CapturingWebSocket` /
`CapturingHttpTransport` decorators (`source/capture.hpp/.cpp`) via `--capture`.

---

## UAT Blockers

**BLOCKER-1 (resolved):** `IxWebSocket` implemented via FetchContent `machinezone/IXWebSocket`. End-to-end connection to live UAT not yet verified.

**BLOCKER-2 (open — narrowed 2026-06-29):** A live `--capture` run against demo
showed network + clock are fine and **public** REST (`GET /orderbook`) returns
200, but every **authenticated** call (`GET /portfolio/positions`, the WS
handshake) returns `401 INVALID_PARAMETER`. Root cause: the `api_key` (access key
ID) in `config-demo.json` is still an unfilled placeholder (`<…>`, not a UUID) —
the private key `.pem` is real, the access key ID is missing. **Action: paste the
real demo access key ID into config, then re-run `--capture`.** Secondary suspect
if 401 persists after that: RSA-PSS salt length in `auth.cpp` uses
`RSA_PSS_SALTLEN_MAX`; Kalshi's SDK uses digest length (32) — verify live once a
real key exists. Field-shape drift (price names, `count` vs `quantity`, status
strings, timestamps) can only be confirmed once authenticated traffic flows.

**Pre-UAT checklist:**
- [x] `IxWebSocket` implemented and library fetched
- [x] Demo RSA private key present (`/home/jfreun1/kalshi-demo-private-key.pem`)
- [ ] Real demo **access key ID** filled into `config-demo.json` (`api_key`) — currently a placeholder
- [x] `config-demo.json` points at demo base/ws URLs
- [~] Raw REST/WS bodies captured via `--capture` (public REST verified 200; authenticated blocked on the key above)
- [x] Paper mode (`--paper`) runs without errors (fixed 2026-06-29 — was silently placing zero orders against the V2 API)

---

## Completed Phases (1–20)

| Phase | Component | Key files |
|---|---|---|
| 1 | Types & Domain Model | `source/types.hpp` |
| 2 | Authentication | `source/auth.hpp/cpp` |
| 3 | REST Client | `source/rest_client.hpp/cpp`, `source/http_transport.hpp` |
| 4 | Local Orderbook | `source/orderbook.hpp/cpp` |
| 5 | WebSocket Client | `source/websocket_client.hpp/cpp` |
| 6 | Order Manager | `source/order_manager.hpp/cpp` |
| 7 | Risk Manager | `source/risk_manager.hpp/cpp` |
| 8 | Fair Value Engine | `source/fair_value.hpp/cpp` |
| 9 | Quoter | `source/quoter.hpp/cpp` |
| 10 | Main Loop | `source/main.cpp` |
| 11 | Pluggable Pricing Model | `source/pricing_model.hpp/cpp` |
| 12 | Theo Grid | (in quoter) |
| 13 | Constraint Bitset & AdverseSelectionGuard | `source/quoter.hpp` |
| 14 | Logging & Observability | spdlog structured logging |
| 15 | Config File & Graceful Shutdown | `source/config.hpp`, `config.example.json` |
| 16 | CI Pipeline & Coverage | `.github/workflows/`, `cmake/coverage.cmake` |
| 17 | Benchmarking | `bench/` |
| 18 | Replay & Fuzz Testing | `test/fuzz/`, `test/fixtures/` |
| 19 | Paper Trading Mode | `--paper` flag |
| 20 | Documentation | `docs/`, `docs/adr/` |

272 tests passing. Build clean.

### Also shipped (post-phase-20, on top of the table above)

| Area | What | Key files |
|---|---|---|
| Ticker Scanner (Phase 31) | ranks markets, writes ready-to-run trade config | `ticker_scanner.*`, `scan_output.*` |
| Portfolio read-model | total realized + unrealized PnL, per-event risk | `portfolio.*` |
| Global kill-switch | `kOverExposure` (capital cap) + `kPortfolioLoss` (realized+unrealized loss floor) + `kDrawdown` (give-back from PnL high-water mark) halt **all** quoting; sampled ~1s | `risk_manager.*`, `RiskManager::update_portfolio` |
| Reconciliation | local vs exchange positions; `kModelDiverge` halt; `--reconcile` | `portfolio.cpp::reconcile` |
| TradingSession engine | domain reactions extracted from `main.cpp` (testable, replayable) | `trading_session.*` |
| Replay integration test | full-stack replay of a session through the real wiring (gated `KALSHI_INTEGRATION_TESTS`, default ON) | `test/integration/replay_session_test.cpp` |
| Session capture | `--capture <dir>` tees raw WS frames + REST responses for replay/UAT | `capture.*` |
| Paper-mode V2 fix | `PaperTransport` now speaks the V2 order schema (was silently broken) | `paper_transport.cpp` |

---

## Next Steps

### Phase 31 — Ticker Scanner

Scans `GET /markets` at startup, scores markets, returns ranked list. Operator picks tickers to add to `config.json`.

```cpp
struct ScannerConfig {
  int min_price_cents{15};
  int max_price_cents{85};
  int min_spread_cents{3};
  int max_spread_cents{10};
  double min_volume_usd{5000.0};
  int min_days_to_close{1};
  int max_days_to_close{10};
};

struct MarketScore {
  std::string ticker;
  std::string title;
  std::string category;
  int mid_price_cents;
  int spread_cents;
  double volume_usd;
  double days_to_close;
  double score;
};

class TickerScanner {
public:
  explicit TickerScanner(RestClient &rest, ScannerConfig config = {});
  [[nodiscard]] std::vector<MarketScore> scan(int top_n = 20) const;
private:
  [[nodiscard]] double score(const MarketScore &m) const;
  RestClient &rest_;
  ScannerConfig config_;
};
```

Scoring (additive, terms normalized to [0,1]):
```
score = 0.35 × log(volume) / log(max_volume)
      + 0.25 × (1 − |mid − 50| / 35)     // peaks at 50c
      + 0.20 × (1 − |spread − 5| / 5)     // peaks at 5c
      + 0.10 × (1 − days_to_close / 10)
      + 0.10 × category_bonus              // Financials=1.0, Econ=0.8, Crypto=0.7, other=0.5
```

**Files:** `source/ticker_scanner.hpp`, `source/ticker_scanner.cpp`, `test/source/ticker_scanner_test.cpp`

---

### Phase 29 — Price-Range Gate — built

Enforced at the single risk chokepoint rather than in the Quoter: `RiskLimits`
gained `min_quote_price_cents` / `max_quote_price_cents` (default `[10, 90]`,
configurable under `risk`), and `RiskManager::check_order` now uses its
previously-unused `price_cents` arg to reject any order whose **own-side**
contract price falls outside the band. Because `check_order` runs before every
`place`, both YES and NO quotes are gated by their own contract price — a YES bid
at 5c and a NO order at 5c (= YES 95c) are both refused. The low bound avoids
cheap longshots (Bürgi: maker returns on <10c are significantly negative); the
high bound caps near-settled extremes. Cancels are unaffected (they don't pass
through `check_order`), so out-of-band resting orders can always be flattened.

**Files:** `source/risk_manager.hpp/cpp`, `source/config.cpp`, `config.example.json`,
tests in `risk_manager_test` + `config_test` (the extreme-inventory `quoter_test`
uses a `[1, 99]` band so it still verifies clamping math).

---

### Phase 27 — Spread Floor & E_win Tracking

Add `min_spread_cents{3}` to `QuoterConfig`. In `Quoter::compute_quotes()`:
```cpp
half_spread = std::max({kHalfSpreadMin, config_.target_spread_cents / 2,
                        config_.min_spread_cents / 2});
```

Add to `OrderManager`:
```cpp
struct ExposureDecomposition {
  double c_a_cents{};   // net cashflow Yes (spread capture)
  double c_b_cents{};   // net cashflow No  (spread capture)
  double e_win_cents{}; // directional exposure on winning outcome
};
[[nodiscard]] ExposureDecomposition exposure(const std::string &ticker) const;
```

**Files:** `source/config.hpp`, `source/order_manager.hpp/cpp`

---

### Phase 26 — Flow Imbalance Signal

```cpp
class FlowImbalanceGuard {
public:
  void record_fill(const std::string &ticker, Side side, int quantity,
                   TimePoint tp = Clock::now());
  [[nodiscard]] double imbalance_ratio(const std::string &ticker) const; // 1.0 = balanced
  [[nodiscard]] bool is_imbalanced(const std::string &ticker) const;
  void reset(const std::string &ticker);
};
```

`Quoter::update()` adds `imbalance_spread_cents` (a `QuoterConfig` field) when `is_imbalanced()` returns true.

**Files:** `source/flow_imbalance.hpp/cpp`, `test/source/flow_imbalance_test.cpp`

---

### Phase 28 — View-Based Pricing (β=0.09 debiasing)

```cpp
class ViewBasedModel : public IPricingModel {
public:
  explicit ViewBasedModel(double view_probability);
  void update_view(double new_probability);
  [[nodiscard]] double estimate(const FairValueInput &input) const override;
private:
  double view_probability_;
};
```

When bootstrapping from market mid, apply debiasing: `π* = (P − 0.045) / 0.91` (derivation: Bürgi et al. β=0.09 calibration). Clamp to [0.01, 0.99].

**Files:** `source/pricing_model.hpp/cpp`, `test/source/view_based_model_test.cpp`

---

### Phase 30 — Maker Fee Integration

After April 2025, Kalshi charges Makers. Confirm γ_maker from current fee schedule. Add `maker_fee_rate` to `Config`. In `Quoter::compute_quotes()`, subtract `γ_maker × P × (1−P)` from effective half-spread so net-of-fee edge stays positive.

---

## Pre-Live Fixes (before first real-money session)

### Code gaps

| Gap | Status |
|---|---|
| Structured logging (every fill / quote / risk state change) | ✅ done — spdlog throughout `TradingSession` + `main.cpp` |
| WS thread can silently stall (no data, no disconnect) | ✅ done — `check_ws_staleness` sets `kStaleBook` after 30s |
| Cancel-all on WS disconnect | ✅ done — `on_disconnect` → `TradingSession::on_disconnect` → `cancel_all` |
| PnL persists across restarts | ✅ done — `persist_pnl`/`load_pnl` (`pnl_state.json`), wired as the session's fill listener |
| Paper mode placed zero orders against V2 API | ✅ fixed 2026-06-29 — `PaperTransport` parses the V2 request + returns the V2 response |

### Missing tests

| Gap | Status |
|---|---|
| Full-stack integration test | ✅ done — `replay_session_test` drives the real wiring (gated `KALSHI_INTEGRATION_TESTS`, default ON) |
| Capture real sessions for replay / field-shape checks | ✅ tooling done — `--capture`; **blocked on the placeholder `api_key`** (see BLOCKER-2) for a real demo capture |
| Replay fixture is hand-crafted, not from live Kalshi | ⏳ pending a real capture — then drop `session.jsonl` into `test/fixtures/` and give `replay_session_test` capture-specific assertions (current ones are tied to the synthetic fixture) |

### Operational hardening (Phase 32)

Deploy as a `systemd` service with auto-restart, add WS staleness detection, persist PnL across restarts.

**`/etc/systemd/system/kalshi-mm.service`:**
```ini
[Unit]
Description=Kalshi Market Maker
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/path/to/kalshi-mm /path/to/config.json
Restart=on-failure
RestartSec=10s
StandardOutput=append:/var/log/kalshi-mm/app.log
StandardError=append:/var/log/kalshi-mm/app.log

[Install]
WantedBy=multi-user.target
```

**`/etc/logrotate.d/kalshi-mm`:**
```
/var/log/kalshi-mm/app.log {
    daily
    rotate 14
    compress
    missingok
    notifempty
}
```

**Files:** `main.cpp` (logging + staleness watchdog + disconnect handler), `source/order_manager.cpp` (PnL persistence), `scripts/install-service.sh`

---

## Monitoring (24/7)

### Minimum viable stack

| Layer | Tool | What it catches |
|---|---|---|
| Process watchdog | systemd `Restart=on-failure` | Crash / OOM |
| Log alerting | cron script → Telegram/email | `[critical]` log lines (risk halt, stale WS) |
| Stale WS detection | `kStaleBook` constraint (in-process) | Silent WS hang |
| Position snapshot | Log net position per ticker every 60s | Inventory drift |
| Daily loss persistence | PnL JSON file | Loss limit surviving restarts |

### Alert triggers to implement

1. **Process not running** — external cron, every 5 minutes, checks `systemctl is-active kalshi-mm`
2. **Risk halt** — any `is_halted()` logs at `critical` level; alert on that pattern
3. **WS silent > 30s** — sets `kStaleBook`, logs at `critical`
4. **Position > 80% of limit** — log at `warn` so you can intervene before halt

### Telegram alert script (simplest path to mobile push)

```python
#!/usr/bin/env python3
# scripts/alert.py — called by cron or log monitor
import subprocess, requests, sys
BOT_TOKEN = "..."
CHAT_ID   = "..."
msg = sys.argv[1] if len(sys.argv) > 1 else "kalshi-mm alert"
requests.post(f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage",
              json={"chat_id": CHAT_ID, "text": msg})
```

Cron entry (checks every 5 minutes):
```cron
*/5 * * * * systemctl is-active --quiet kalshi-mm || python3 /path/scripts/alert.py "kalshi-mm is DOWN"
```

---

## Rate Limiting

Kalshi Basic tier: **200 read tokens/s**, **100 write tokens/s**. Each REST request costs 10 tokens; batch cancels cost **2 tokens** each. Basic tier has no burst (1-second bucket only).

**At ≤5 tickers on slow prediction markets:** safe. A reprice = 1 cancel (2 tokens) + 1 place (10 tokens) × 2 sides = ~24 write tokens. Need >4 reprices/second/ticker to blow the budget — won't happen on event contracts.

**Risk points:**
- Startup: seeding N orderbooks = N GETs simultaneously. Fine at ≤5.
- Fast-moving market (e.g. Fed day): if BBO ticks every second, the `reprice_threshold_cents` config is the main protection — don't reprice unless BBO has moved ≥1c. Already implemented.
- If 429 responses appear: log them, add a per-ticker cooldown timer (skip reprice for 500ms after a 429).

**When scaling beyond Basic:** target the Advanced tier (300/300) or use the `POST /portfolio/orders/batches` endpoint for bulk placement when Phase 21 (async dispatch) is implemented.

---

## Deferred — Scaling (revisit after consistent profit on ≤5 tickers)

Scalability is a goal, but the bottlenecks below only matter once pricing is working and generating edge. Expand to these only after the small-ticker setup is demonstrably profitable. Long-term, the same architecture can extend to **Polymarket and other prediction market exchanges** — the `IHttpTransport` and `IWebSocket` interfaces are designed for exactly this: swap in a Polymarket REST/WS implementation behind the same interfaces, reuse `OrderManager`, `RiskManager`, and `Quoter` unchanged.

### Target architecture: process-per-strategy + an aggregator process (see [ADR-007](docs/adr/007-process-per-strategy-and-aggregator.md))

The end-state for scaling is a **portfolio of strategies** with the quoting layer
separated from the risk-aggregation layer, as distinct OS processes:

- **Each market maker / Quoter is its own process** — one strategy over a market
  set, its own exchange connections, enforcing *local* risk. This is exactly a
  `TradingSession` + its transports.
- **A "portfolio of portfolios" aggregator is its own process** — consumes every
  quoter's `RiskReport`, enforces *global* risk + capital allocation, and emits
  `ControlCommand`s (halt/resume/limit). This is today's in-process global
  kill-switch (`Portfolio` + `RiskManager::update_portfolio`) promoted to a
  process with many inputs.

A second driver is **multiple exchanges**: a quoter process targets one venue via
its own `IHttpTransport`/`IWebSocket` adapter (Kalshi today, **Polymarket** next),
and the aggregator becomes a cross-exchange risk/arbitrage authority — netting
exposure and hedging across venues, and acting on the same event priced
differently on each. Polymarket is on-chain (EVM) with very different auth,
latency, and settlement, so its own process keeps those quirks off the Kalshi hot
path while it reports into the same aggregator via the same `RiskReport`.

Boundary: `IRiskPublisher` (quoter → aggregator, payload ≈ `PortfolioSnapshot` +
`strategy_id` + heartbeat) and `IControlChannel` (aggregator → quoter), in-process
today, IPC at split time — same interface+fake discipline as `IHttpTransport`.
**Already positioned:** `TradingSession` is the quoter core, aggregation already
consumes a `PortfolioSnapshot` DTO (not live objects), and `IPricingModel` is the
strategy seam. **Don't regress:** keep the aggregator snapshot-only (never reach
into a quoter's internals); route remote halts through `RiskManager` +
`enforce_quote_safety` so the cancel-on-halt invariant holds across the wire.
Phase 24 below *is* the aggregator extraction; Phase 25 lives in it.

| Phase | Component | Bottleneck it solves |
|---|---|---|
| 21 | Async HTTP Order Dispatch | REST blocks reprice at ~5 tickers |
| 22 | Per-Series WS + Thread-per-Series | Single WS thread serializes all repricing |
| 23 | Incremental RiskManager Update | O(n) scan on every fill |
| 24 | Aggregator process (PortfolioModel + global risk) | Portfolio of strategies needs one risk/PnL authority across processes |
| 25 | Cross-Ticker Delta Hedging (in the aggregator) | Unhedged directional exposure across series/strategies |
| 26+ | Multi-Exchange Support (Polymarket, etc.) | New exchange adapters behind existing interfaces |

### Portfolio aggregation (read-model) — built

`Portfolio` (`source/portfolio.hpp/.cpp`) is a pure read-model over `IOrderManager`:
given a ticker universe and a mark map (ticker → YES mid cents), `snapshot()`
returns total realized PnL, total **unrealized** (mark-to-market) PnL, total
capital at risk, and a per-**event** breakdown (correlated strikes rolled up via
`event_ticker_of`, sorted by capital at risk). `OrderManager` gained
`unrealized_pnl(ticker, yes_mid)` and `position_cost(ticker)` to source the
mark-to-market and capital-at-risk numbers from its open lots. The main loop logs
the aggregate each status interval. This is the fan-in backbone the per-strategy
quoter processes will report into once aggregation moves to its own process (see
the Target architecture above + [ADR-007](docs/adr/007-process-per-strategy-and-aggregator.md)).

**Portfolio-level safety (built on top):**
- **Global halt (kill-switch)** — `RiskManager::update_portfolio(const PortfolioSnapshot&)`
  consumes the read-model (the single aggregation authority) rather than re-summing
  positions, and trips bits that halt **all** quoters at once (`check_order`
  returns false on any set bit):
  - `kOverExposure` when `snapshot.total_notional_cents` exceeds
    `risk.max_total_exposure_dollars` — per-market limits don't bound aggregate
    exposure at scale.
  - `kPortfolioLoss` when `snapshot.total_pnl_cents()` (realized **+** unrealized
    mark-to-market) falls below `risk.max_total_loss_dollars`. The realized-only
    `daily_loss_limit` / `kPnLLimit` would miss a book bleeding while holding
    inventory; this watches the absolute-loss floor the read-model exists to surface.
  - `kDrawdown` when total PnL has given back more than `risk.max_drawdown_dollars`
    from its **session high-water mark**. Unlike the loss floor (anchored at
    break-even), this protects gains — it can fire while still net profitable. The
    peak starts at 0 and `resume()` re-anchors it so a manual resume doesn't
    instantly re-trip.

  All only set bits; clearing requires `resume()` (don't auto-resume into a
  crashing market). The main loop builds the snapshot once and feeds it to both
  the kill-switch (every ~1s, `run_portfolio_tasks`) and the status log (~60s).
  Truly event-driven (recompute on every WS delta) is deferred to Phase 23
  (Incremental Risk) — full recompute per delta doesn't scale; 1s sampling does.

  ```mermaid
  graph TD
    OM[OrderManager<br/>single source of truth] -->|net pos, lots, realized| PF[Portfolio<br/>read-model]
    OB[Orderbooks] -->|marks| PF
    PF -->|PortfolioSnapshot| RM[RiskManager.update_portfolio]
    RM -->|notional > cap| OE[kOverExposure]
    RM -->|realized+unrealized < loss cap| PL[kPortfolioLoss]
    OE --> HALT[is_halted = any bit set]
    PL --> HALT
    HALT -->|check_order = false| Q[ALL Quoters stop]
    style OM fill:#555
    style OB fill:#555
    style PF fill:#555
  ```
- **Reconciliation** — `reconcile()` (portfolio.cpp) diffs local net positions
  against the exchange's authoritative `GET /portfolio/positions`
  (`RestClient::get_positions`, paginated). Checks the union of tracked tickers and
  any ticker the exchange reports a non-zero position in (catches positions we
  don't know about). On drift it trips `kModelDiverge` and halts. Runs every ~2min
  live; also a standalone `--reconcile` command (exit non-zero on mismatch) for
  pre-trade / CI checks.

Next: optional auto-resync of local state from the exchange snapshot, and wiring
the shared kill-switch into the sharded quoters once Phases 21–22 land.

---

## Research Findings

### Bürgi, Deng, Whelan 2026 — Key Numbers

| Metric | Value |
|---|---|
| Avg return all contracts | −20% pre-fee |
| Maker avg return | −9.64% |
| Taker avg return | −31.46% |
| Maker return on ≥50c | **+2.6%** (stat. sig.) |
| Maker return on <10c | ~−35% |
| Taker fee formula | γP(1−P), γ=0.07 pre-Apr 2025 |
| Belief bias β | 0.09 (range 0.06–0.12) |
| Maker match rate θ | 0.60 |
| Belief dispersion σ | 0.107 |
| Std dev Maker returns ≥50c | 33% |

**Debiasing formula:** `π* = (P − 0.045) / 0.91`

| Market P | Debiased π* |
|---|---|
| 5c | 0.5% |
| 20c | 17% |
| 50c | 50% |
| 80c | 83% |
| 95c | 99.5% |

**Design rules from paper:**
- Quote ≥15c only — Maker losses below 10c are statistically significant negative
- Prefer Financials, Economics, Crypto categories (larger volume, lower ψ bias coefficients)
- `post_only=true` already correctly positions every order as Maker
- Equilibrium spread at θ=0.60 is 3–5c at mid-range → `min_spread_cents=3` floor

### Palumbo 2026 — Key Finding

LPs accumulate net directional exposure (`E_win`) that dominates terminal P&L. This is underwriting, not spread capture. Flow imbalance (winner-to-loser volume ratio) is the single largest predictor (coeff −3.13 for assets, +2.63 for liabilities). Fill rate alone is insufficient — need side-weighted volume imbalance tracking (Phase 26).

---

## Dependency Summary

| Library | Purpose |
|---|---|
| OpenSSL | RSA-SHA256 signing |
| cpp-httplib | HTTPS REST client |
| IxWebSocket | WebSocket (FetchContent) |
| nlohmann/json | JSON parsing |
| spdlog | Structured logging |
| Google Test | Unit tests |
| Google Benchmark | Microbenchmarks |
| libFuzzer | Fuzz testing |
| lcov | Coverage reports |

---

## Phase Checklist

- [x] Phases 1–20 — complete (272 tests passing)
- [x] Phase 31 — Ticker Scanner
- [x] Portfolio read-model + global kill-switch (`kOverExposure` + `kPortfolioLoss` + `kDrawdown`) + reconciliation
- [x] TradingSession engine extracted from `main.cpp`
- [x] Full-stack replay integration test + `--capture` mode (paper-mode V2 bug fixed en route)
- [x] Pre-live fixes — logging, WS staleness, cancel-on-disconnect, PnL persistence

**Immediate (pricing quality, small ticker set):**
- [~] UAT Blocker — `--capture` built & run; **blocked on placeholder `api_key` in `config-demo.json`** (fill real access key ID, then capture + verify field shapes). See UAT Blockers.
- [ ] Real-capture replay — record a demo session, drop into `test/fixtures/`, add capture-specific assertions to `replay_session_test`
- [ ] Phase 32 — Operational hardening (systemd, logrotate, Telegram alert script)
- [x] Phase 29 — Price-Range Gate (band gate in `check_order`, default [10,90]c)
- [ ] Phase 27 — Spread Floor & E_win Tracking
- [ ] Phase 26 — Flow Imbalance Signal
- [ ] Phase 28 — View-Based Pricing (β=0.09 debiasing)
- [ ] Phase 30 — Maker Fee Integration

**Deferred (scaling — after consistent profit on ≤5 tickers):**
- [ ] Phase 21 — Async HTTP Order Dispatch
- [ ] Phase 22 — Per-Series WS + Thread-per-Series Dispatch
- [ ] Phase 23 — Incremental RiskManager Update
- [ ] Phase 24 — PortfolioModel
- [ ] Phase 25 — Cross-Ticker Delta Hedging
