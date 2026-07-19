# Kalshi Market Maker — Build Plan

> Lean plan, updated 2026-07-18. Full history (every completed item with its
> validation notes, runs 1–20 detail, findings D1–D14, external-review
> reconciliation):
> [docs/archive/PLAN-2026-07-18-full.md](docs/archive/PLAN-2026-07-18-full.md)
> — supersedes the 07-03/07-04 archives. Research:
> [docs/papers](docs/papers/README.md) · architecture:
> [docs/architecture.md](docs/architecture.md) · guards catalog:
> [docs/GUARDS.md](docs/GUARDS.md) · API ground truth:
> [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md) · pricing
> research: [docs/BETTER_PRICING.md](docs/BETTER_PRICING.md) · market
> structure & strategy taxonomy:
> [docs/MARKET_STRUCTURE.md](docs/MARKET_STRUCTURE.md).

## Gates (Jacob's calls — do not relitigate)

- **Gate 1**: no real money until demonstrably profitable in demo, judged by
  the pre-declared thresholds in
  [docs/PRE_LIVE_CHECKLIST.md](docs/PRE_LIVE_CHECKLIST.md) (**still DRAFT —
  Jacob must ratify §2**).
- **Gate 2**: no scaling / multi-exchange until profitable on Kalshi. One
  market at a time until the edge is proven.
- **North-star**: FIX access — requires ≥5% of exchange-wide monthly volume
  (Kalshi Institutional, 2026-07-04). A scale gate, not a tech unlock; the
  road is Gate 1 → Gate 2 → volume. REST+WS is our platform for the
  foreseeable future.
- **Boundary**: host health (clock sync, disk, OS) is ops' job, not bot code
  (item 46 closed unmerged on this principle).
- **Hosting gate (2026-07-18)**: no L1/EC2 spend until either the bot is
  demonstrably profitable on the current machine, or the measurement backbone
  proves latency is where the money is lost (attribution shows losses
  concentrated in adverse selection / stale-quote fills that the ~294ms→Ohio
  RTT cut would plausibly fix).

## Now (ordered)

1. [ ] **19 — Jacob ratifies Gate-1 thresholds** (PRE_LIVE_CHECKLIST §2);
   the 14-session/30h measurement window can't start until frozen.
2. [ ] **73 — REST fill backfill on reconcile drift (run 22, 2026-07-18).**
   1.20 contracts of maker fills never arrived on the WS fill channel: the
   exchange showed net 5.00 vs local 6.20 and the session (correctly) halted
   on `kModelDiverge` for its remaining 17 minutes. On drift, fetch
   `GET /portfolio/fills` since session start, replay missing fills through
   `record_fill` (idempotent via trade_id dedup), re-reconcile, and halt only
   if still diverged. Wire the same backfill into the WS `on_reconnect` hook
   (fills during a connection gap are pushed only once). Revives the closed
   #58 idea, now with a measured live cause.
3. [ ] **74 — shutdown flatten retries the unfilled remainder (run 21,
   2026-07-18).** The final taker IOC filled 3.00 of net 5.00 and never
   retried — 2 contracts rode to settlement unmanaged. Retry the remainder
   at a re-fetched top-of-book price (small bounded loop), and log a loud
   `CARRIED` line with the residual when still short at exit.
4. [ ] **Phase 32 minimum — unattended supervision on the CURRENT machine**
   (systemd + logrotate, Telegram alert on halt/error) — required for the
   30h soak; nothing here needs the cloud (see Hosting gate).
5. [~] **31a/31c — measurement backbone.** *31a done 2026-07-18 —
   `scripts/settlement_join.py`: every fill revalued at the settled outcome
   (ground-truth PnL vs the marked figure), time-weighted Brier of fv / mid /
   micro against the outcome (the Hosting-gate discriminator: fv beating mid
   = latency/queue problem, fv losing = pricing problem), calibration buckets
   accumulating across sessions, outcomes cached.* Remaining 31c residual:
   fees term once maker-fee markets are traded.
6. [ ] **L1 — EC2 in Kalshi's region** (t4g.small ~$12/mo). **Blocked by the
   Hosting gate.** Region answered by Kalshi Institutional (2026-07-11):
   us-east-2 (Ohio). When unblocked: rerun the L0 latency session from Ohio
   (Mac baseline to beat: order-path ~294ms, orderbook ~76ms); fold the host
   into Phase 32 supervision. Rate-limit ladder above Advanced: Premier
   1k/1k · Paragon 2k/2k · Prime 4k/4k per sec, unlocked by volume share —
   the write-budget ladder for items 54/L3.
7. [ ] **L3 — async order dispatch** (sanctioned Phase-21 pull-forward):
   order REST calls block the WS thread today; the bot is deaf while one is
   in flight.
8. [ ] **L4 — timer teardown.** After echo-free amends AND item-1 validation
   hold: shrink `min_rest_ms`/`fade_rest_ms` toward zero (config-only).
   Governors that remain: reprice threshold, write budget, RTT.
9. [ ] **51 — panic pull tier (Jacob to confirm)**: on a catastrophic jump
   (≥ `panic_jump_cents`, e.g. 8), cancel the toxic side instantly — no
   confirmation, no rest floor, no re-quote until the book settles.
10. [ ] **60 — regression calibration.** (a) **Drift estimator — build
    first, no fill history needed**: significance-gated rolling regression
    (slope + t-stat) of the smoothed mid on the quote stream; scale the flow
    lean (`clamp(k·slope·horizon, ±max)`) and penalize |slope| in the
    scanner — trending books are where makers bleed; prefer two-sided chop.
    (b) **Fill-history models — gated on ~500 accumulated fills**: toxicity
    regression → per-fill required-edge floor; learned micro-price; logistic
    fill-probability (unblocks 42b and Kelly sizing). Every soak session's
    JSONL is the training set.
11. [~] **66 — clearing-price fair value.** Phases 0–4 shipped (WS trade
    channel + `TradeTape`; `clearing_price_cents` full-ladder uncross;
    `ClearingPriceModel` = (1−w)·EMA-micro + w·tape-VWAP, OFF by default;
    `--fv-replay` 39-candidate offline harness). **Blocked on data: needs a
    `--capture` recording of a live slate** to pin `clearing_tape_weight`;
    then a run-20-style matched A/B decides the live flip. Full plan and
    backtest results: [docs/BETTER_PRICING.md](docs/BETTER_PRICING.md).
12. [ ] **24 — layered quoting (queue priming)**: 2–3 size layers 1–3c
    behind the inside; queue position is earned by resting before the move.
13. [ ] **42b — queue-value reprice rule**: only reprice when
    `|fv − quoted| · fill_prob` beats the queue value abandoned (needs 31
    fill-prob data).
14. [ ] **45 — decision-oriented quote logging** (the "why" per placement:
    spread components, reprice reason; latency counters fold into L0; plus
    the avg-entry/mark/edge per-fill status fields from item 59's residual).
15. [~] **59 — short-horizon markout.** *Script half done 2026-07-18 —
    `analyze_fills.py` reports markout@500ms/1s/5s alongside 30s/5min, a
    signed per-fill edge, and edge+markout by pre-fill inventory bucket.*
    Remaining: the per-fill status-log fields (folds into item 45).
16. [ ] **58 — scanner startup efficiency.** Startup scans ~70k markets for
    ~30s before quoting. Query `status=open` server-side is already used;
    cap pagination and/or cache market metadata between runs with
    incremental refresh. Becomes real waste on every rotation re-scan.
17. [ ] **75 — committed `config.json` should ship `target_tickers: []`**
    (self-selection is the documented default; the four pinned CPI tickers
    are stale) and the run scripts should agree on one canonical local
    config name (`config-demo.json` today, absent on fresh clones).
18. [ ] **23 — ceil-per-order maker-fee model** (inert while demo maker
    fills are free) · **3 — passive clamp vs fresher BBO** (D1 residual).
19. [ ] **54 — batch CreateOrders.** V2 batch create/cancel; batch seeds and
    layered quotes into one request. Sequence after L3 so batching composes
    with the non-blocking order path.
20. [ ] **55 — demo conformance suite in CI** (nightly + manual dispatch,
    not per-PR; needs demo creds as Actions secrets; non-required job — red
    means schema drift or demo outage, both alert-worthy). Skips are
    failures (Jacob): tests self-find a market; only missing creds may skip.
21. [ ] **69 — per-market precision audit.** Subpenny pricing is per-market
    (`price_level_structure`, ignored today); quantities are fixed-point 2dp.
    Audit price-tick and quantity-step per market, carry a `MarketPrecision`
    through types, pin live demo behavior in conformance tests before any
    live switch on a subpenny market.
22. [ ] **70 — max-hold forced exit** (`max_hold_seconds`, 0 = off): passive
    exit first, forced taker exit at the deadline — bounded fee in place of
    unbounded z² drift (ND-HFTT pattern; docs/papers §6). Tune against
    attribution: right when drift saved exceeds taker fee + spread paid.
23. [ ] **71 — crypto 15m series (KXBTC15M family).** Measured on demo
    2026-07-11: ~130 trades/hr, two-sided, 24/7 — the most quotable flow
    demo has, excluded only by `min_days_to_close`. Build: per-window ticker
    resolution + 15-min session lifecycle (stop quoting ~60s before close;
    positions settle 0/1); quote the 10–90c middle band until item 69 lands
    (tapered_deci_cent tails); external reference feed (Coinbase leads
    ~1–2s) for the momentum cancel. Risk: terminal z is structural — quote
    early/mid window, flee the end.
24. [ ] **72 — validate the backtest fill model against captured tape.**
    Match trade prints to negative top-of-book deltas (±50ms, ND-HFTT
    method) on our capture corpus to measure print-through under-fill;
    adopt proportional delta-consumption if the gap is material — guards
    the clearing-pricing verdict from a flattering fill model.
25. [~] **76 — modularization refactor (Jacob, 2026-07-18): layered modules,
    swappable strategy — ACTIVE FOCUS.** End state: `engine/` (session, risk,
    execution interface, pricing, market data) reusable with any strategy or
    venue; Quoter is one `IStrategy` implementation. Sequence: IOrderManager
    interface split (PR 1) → IStrategy seam (PR 2) → layer directories +
    per-layer CMake targets with build-enforced DAG (PR 3) → include lint
    (PR 4) → config-struct extraction (PR 5, after item 75). Explicitly
    skipped: rest_client/quoter splits (quoter is about to be rewritten for
    the one-sided-flow problem), ADR-007 process split.

**Selection principle (Jacob, 2026-07-04): profitable on every market we
CHOOSE, then scale.** Not every market can be made profitably — trending
books, dead books, and 1c-spread deep books all bleed makers. The bar: only
quote markets where measured expected edge is positive (liveness ✓, spread
band ✓, drift penalty = 60a, toxicity floor = 60b), and exit any market whose
live attribution turns negative (rotation provides the mechanism).

**Situational** (apply when relevant): 26 √-time size taper · 27 closing-day
longshot guard · 28 quarter-Kelly sizing (gate on 31) · 30 per-category
debias β · 34 sum-to-one monitor · 36 scanner volume-cap.

**Production readiness** (after Gate-1 definition): real-capture replay
fixture · 35 per-market position cap · 6 UBSan/TSan CI jobs · 7 tooling
audit · scanner in-play config flag (D8 follow-up).

## Gated behind Gate 2 (do not start)

Phase 21–26 scaling architecture, multi-exchange, FIX transport (see
north-star), `Session` concept — detail in the archive and
[ADR-007](docs/adr/007-process-per-strategy-and-aggregator.md). P3 structural
refactors (R1–R4, R7) never block the gates.

## Done since the 2026-07-05 refresh (validation notes in the archive)

31a settlement join + Brier scoring (07-18) · 59 short-horizon markout script
half (07-18) · reconcile baseline for shared-account leftovers (#124, 07-18)
· feed-liveness gate: never quote a never-ticked book (#125, 07-18) · WS
late-subscribe fix — rotation-adopted markets now actually subscribed (#126,
07-18) · Hosting gate recorded (#121) · 64 asymmetric unwind pricing · 65
two-sided-flow admission · 67 reversion-κ admission (Chakraborty–Kearns) ·
68 K/z² attribution split · 63 pre-game tape lookback · BETTER_PRICING
phases 0–4 (TradeTape, clearing price, ClearingPriceModel, --fv-replay) ·
live-harvester + K/z study docs (#117–#119) · config/secrets split (#120).
Earlier waves (L0/L2, items 32/49/52/53/56/57/61/62, runs 12–16) are in the
07-04 archive.

## State of play (2026-07-18)

- **Quoter/guards** (full catalog + provenance: docs/GUARDS.md): EMA fv +
  min-rest + adverse fade + LMSR skew (25×quote_size floor) + longshot floor
  + flow lean + inventory brake + asymmetric unwind + wind-down +
  maker-favor rounding + crossed-book guard + own-quote subtraction +
  feed-liveness gate (#125).
- **Runs 21/22 (2026-07-18, WSL machine, first sessions since 07-12).**
  Run 21 (−$1.08) exposed two infrastructure faults: rotation-adopted
  markets received NO WS data (`subscribe()` after connect never sent the
  command — #126) and nothing stopped quoting on a never-ticked book
  (#125); both losses were frozen-quote pick-offs, not pricing. Run 22 on
  the fixes: every book live (adoption→snapshot ~90ms), first profitable
  round trips (+$0.09 realized by minute 2, unwind pricing completing loops
  at 2–4c/pair), then reconcile caught 1.20 contracts of undelivered WS
  fills and halted — correct behavior, new item 73. The #124 baseline
  carried the week-long unflattenable PGA leftover (−10, closed market,
  settles ~07-26) cleanly all session.
- **Loss taxonomy update**: per-fill pricing earns; the leak channels are
  (a) one-way flow — items 65/66/60a, and (b) now-fixed infrastructure
  faults. Latency remains unproven as a loss driver (Hosting gate).
- **Measurement toolchain complete**: `pnl_attribution.py` (entry/drift/exit
  + picked-off + K/z²) · `analyze_fills.py` (markout 500ms→5min, edge by
  inventory) · `settlement_join.py` (ground-truth PnL + Brier + calibration)
  · `latency_report.py` (L0). 2 contracts of KXMLBKS (Rogers 4+ Ks) carried
  to tonight's settlement = the first live settlement-join datapoint.
- **Ledgers are per-machine** (`pnl_state.json` is local): Mac −$5.22
  through run 19 (07-05); WSL −$0.99 (runs 21/22). PnL needs completed round
  trips; two-sided books remain the binding constraint on demo.
- **Open PRs**: #124 (reconcile baseline) · #125 (feed-liveness gate) ·
  #126 (WS late-subscribe). Validated together live in run 22.
- Demo quirks: order entry can 503 exchange-wide while `/exchange/status`
  says active; fills can be fractional; laptop sleep mid-session is safe but
  wastes the session.

## Working agreements

TDD; one PR per fix; PR descriptions start with a Review-order section;
**no stacked PR chains — sequential PRs on main**; commits ≤15 words, no
attribution; credentials live in each machine's gitignored `secrets.json`
with private keys outside the repo (see CLAUDE.md), never in git; Mermaid
diagrams in docs/architecture.md.
