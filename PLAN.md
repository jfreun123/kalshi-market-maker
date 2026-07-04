# Kalshi Market Maker — Build Plan

> Condensed 2026-07-03. The full pre-collapse plan (all completed phases,
> resolved findings, historical detail, and old diagrams) is preserved verbatim
> in [docs/archive/PLAN-2026-07-03-full.md](docs/archive/PLAN-2026-07-03-full.md).
> Research notes live in [docs/papers/README.md](docs/papers/README.md);
> architecture in [docs/architecture.md](docs/architecture.md); API ground truth
> in [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md).

## Strategy Gates (2026-07-03 — Jacob's call, do not relitigate)

**Gate 1 — no production until profitable in demo.** No real capital until the
bot is demonstrably profitable in the demo environment, measured against
pre-declared PASS/FAIL/KILL thresholds (item 19), on sustained sessions — not
one lucky run.

**Gate 2 — no scaling until profitable on a single exchange.** No multi-market
breadth, no Polymarket / multi-exchange work, no process-per-strategy
architecture until single-exchange (Kalshi) trading is profitable. Focus is
**one market at a time** (`trade_top_n=1` in the demo config) until the edge is
proven, then a small handful.

Everything below is ordered by these gates: first the path to demo
profitability, then production readiness, then (gated) scale.

## Now — Path to Demo Profitability (ordered)

The measured baseline (run 3, 2026-07-03): pure passive quoting on an in-play
market lost **−2.4c/contract** to one-sided flow. Closing that gap is the whole
game. Fixes ship one PR each, TDD, per CLAUDE.md.

### Top priority — the low-latency package (Jacob's call, 2026-07-04)

Quoting competitively on fast markets is a latency problem before it is a
strategy problem. The decision path is already ~300ns (PR #51 benchmark); the
~1s worst-case from "book moved" to "new quote resting" is geography, two
serialized REST calls, and defensive timers that exist only to protect a bot
that is slow and can't tell its own book echo from the market. Attack the
mechanics in leverage order, then delete the defenses:

- [ ] **L0 = item 45 (latency half), pulled forward: instrument + baseline
  BEFORE any change (S).** You can't compare what you didn't measure. Add
  latency counters to the analytics JSONL: REST RTT per order call
  (place/cancel/amend — the transport already times every request at debug,
  `http_transport.cpp` `timed()`; surface it), order ack latency,
  WS-delta→quote-decision gap, and decision→request-sent gap. Extend
  `scripts/analyze_fills.py` (or add `latency_report.py`) to print
  p50/p95/max per stage. Then run **one baseline session on the Mac** and
  record the numbers in the Findings Log. Every later change (L1 VM, L2
  amend, L3 async, L4 timers) is judged as a measured delta against this
  baseline — same script, same stages.
- [ ] **L1 = item 37. Deploy near the matching engine (S, ops).** ~300ms REST
  round trips from the Mac become ~3–5ms from a us-east VM — ~100×, zero code,
  dollars/month. Steps: (a) region probe first — measure RTT to
  `demo-api.kalshi.co` (and prod) from us-east-1 AND us-east-2 and let the
  numbers pick (Kalshi is AWS-hosted US East; third parties claim us-east-2 /
  Ohio, unverified); (b) run the L0-instrumented session from the winning
  region and compare stage-by-stage vs. the Mac baseline; (c) compare
  cross/reject rates and markout; (d) fold the host into Phase 32 supervision
  (launchd/systemd, logs, alerts).
  **Host decision (2026-07-04): small EC2 in Kalshi's own region (t4g.small
  ~$12/mo is ample for a 2-thread, ~300ns-decision bot), NOT a third-party
  "trading VPS".** Evaluated tradoxvps.com: $39–249/mo for Chicago placement
  with a self-benchmarked "~10ms" claim and gaming-CPU specs that are
  irrelevant to a network-bound bot — and their own copy concedes that
  beating ~10ms "requires an instance physically inside AWS us-east-2",
  which is exactly what we rent directly for a fraction of the price.
  Placement beats hardware. Same finding re-affirms the item-11 gate: FIX
  saves per-order milliseconds that only matter once in-region with amend
  (L2) landed, and needs Kalshi-side access — judge it from L0 deltas then,
  not from VPS marketing.
- [ ] **L2 = item 44. Amend + Decrease Order V2 (M).** Replace cancel+replace
  in the reprice branch with a single atomic amend: one round trip (not two),
  cheaper in tokens, no quote-less window, no post-only-cross re-entry risk,
  and very likely no book echo (the D9 root). **Verify semantics empirically
  first** via gated demo-conformance tests: amend price → expect priority
  lost, book deltas coherent (no dead-level echo); decrease size → expect
  `remaining_count` correct, priority kept. Decrease unlocks queue-preserving
  inventory control ("keep the queue, cut the risk"). Detail in item 44 below.
- [ ] **L3 = Phase 21 pull-forward: async order dispatch (M).** Order REST
  calls currently run synchronously on the WS thread — the bot is deaf to
  deltas while an order is in flight. Already sanctioned as a pull-forward in
  *Gated behind Gate 2*; on-VM latency makes this the next bottleneck after
  L1/L2.
- [ ] **L4. Timer teardown — now also gated on item 50 (belief smoothing).**
  Run 7 showed the timers are currently *containing* micro-price noise on
  in-play books; tearing them down before the fair-value anchor is smoothed
  would re-open D9/D13 at full speed. Once L2's conformance tests prove amend is
  echo-free, shrink `min_rest_ms`/`fade_rest_ms` toward zero (config-only) and
  let the real governors govern: `reprice_threshold_cents` for noise, the
  write budget for rate, RTT for physics. The item-42a/33 timers were
  anti-self-harm, not strategy; with sound mechanics they are vestigial.
  Each L-step ships with an L0-comparable measurement so the speedup is a
  number, not a feeling.

Measurement items 19/31 stay live — L1's VM-vs-local comparison is judged on
item-31 markout. Everything else in the ordered list below yields to L1–L4
until the package lands.

1. [ ] **19. Falsifiable edge + PASS/FAIL/KILL thresholds.** State the edge in
   one falsifiable sentence; pre-declare the demo-profitability criteria that
   Gate 1 is judged by; plan live-vs-paper fill-divergence measurement. Output:
   `docs/PRE_LIVE_CHECKLIST.md`. *This defines what "profitable in demo" means —
   do it first.*
2. [~] **31. Measurement backbone: calibration + fill analytics (S/M).**
   *(b) shipped — `AnalyticsLogger` (`source/analytics.{hpp,cpp}`) emits JSONL
   quote-decision events (mid/micro/fv/bid/ask/inventory/imbalanced from
   `Quoter::update`) and fill events (price/qty/fees/mid/inventory-after from
   `TradingSession::on_fill`) to `logs/analytics.jsonl`;
   `scripts/analyze_fills.py` computes per-fill markout @30s/@5min and
   effective spread offline. This is the Gate 1 evaluation input.* Remaining:
   (a) settlement-outcome join + Brier scoring (the P_fair/P_market stream is
   already persisted per decision); (c) PnL attribution split: realized spread
   vs. mark-to-market vs. inventory vs. fees (extends
   `exposure_decomposition`).
3. [x] **22. Round quotes in the maker's favor (S).** *Done — merged PR #53:
   `floor` bid, `ceil` ask.* (Berg & Proebsting pp.53–54.)
4. [ ] **23. Maker-fee widening ON by default + ceil-per-order fee model (S).**
   The documented maker edge predates maker fees (Bürgi p.6); fee rounds up to
   the cent per order (1.77% effective at 50c×100).
5. [x] **33. Cancel-on-theo-jump quote fade (M).** *Done — a fair-value move
   ≥ `theo_jump_cents` (default 3) against a resting order drops that side's
   rest floor from `min_rest_ms` (3000) to `fade_rest_ms` (500): the toxic
   side fades fast, the safe side keeps its queue position, and the fade floor
   stays above the exchange's sub-second echo so D9 stays dead. Directly
   attacks the −2.4c/contract measured in run 3 (Bürgi p.27: the maker edge
   *is* repricing).*
6. [x] **43. Visible-book sanity guard (S — found by external log review).**
   *Done — merged PR #54: update skipped (resting quotes kept) when the visible
   book is crossed (`best_bid ≥ best_ask`), with a warn log.* Original run-3
   evidence: `yes@28`, `yes@29`, bid `55` placed in a market quoting 46–52
   during fast sweeps. Residual (not shipped): implausible-inside-jump check
   vs. the last mid.
7. [ ] **32. Directional flow lean (M).** IR > 0.65 → bounded fair-value offset
   (±1c) or asymmetric size, decaying over 15–30 min (Bawa p.8). Validate
   thresholds via item 31 markout — the headline R² is an equities number.
8. [x] **29. Asymmetric quoting — longshot-side edge floor (M).** *Done — a
   quote that would buy below `longshot_price_threshold_cents` (default 40) is
   shaded by `longshot_edge_cents` (default 1) of extra edge on that side only
   (YES bid below the threshold, or a NO buy whose price is below it); the
   favorite side quotes normally. Thresholds are config knobs to tune from
   item 31 markout data.* Maker returns are negative on all sub-50c buys
   (Bürgi Fig. 6).
9. [x] **25. LMSR log-odds inventory skew (S).** *Done — linear
   `skew_per_contract_cents` replaced with `lmsr_skewed_fair_value` (fv' =
   c/(1+(c/fv−1)·e^{q/b})); `lmsr_b_from_risk` derives b so holding
   `max_position` moves a 50c reservation exactly to the band edge (defaults:
   b = 100/ln 9 ≈ 45.5 → 20 contracts shift fv 52 → 41). Self-attenuates near
   1c/99c, can never leave range; degenerate band (edge ≤ 50c) disables it.
   Fixes the invisible-skew half of D5.* (Berg & Proebsting pp.49–56.)
10. [ ] **24. Layered quoting — queue priming (S/M).** Rest 2–3 size layers 1–3c
   behind the inside: price-time FIFO means queue position is earned by resting
   *before* the move (item 9's measurement: ~115k ahead when joining late);
   sweeps pay progressively more; each layer earns incentive score. Genuine
   intent-to-fill laddering — avoid the word "layering" externally (regulators
   use it for the manipulative variant). Depends on own-quote subtraction
   (shipped, PR #44).
11. [ ] **3. Passive clamp vs. fresher BBO (D1 residual, M) + 37. deploy near
    the matching engine (S, ops) — item 37 elevated to L1 above.** Run 3 measured ~6 `post only cross`
    rejects/10min on an in-play book — the ~300ms local RTT is the cause. Item
    37 (a US-East VM: measure RTT → demo session → compare reject rates) likely
    buys more than any software mitigation and gates whether FIX (item 11) is
    ever worth it.
12. [~] **42. Reprice hysteresis + queue-position value (S/M) — half done.**
    *(a) shipped — `min_rest_ms` (default 3000, config `quoter.min_rest_ms`):
    the reprice branch may not cancel a quote younger than the window;
    placements and out-of-band safety cancels are unaffected. Kills the D9
    echo-race class (self-reference in time).* Remaining — (b) Jacob's
    queue-value rule:
    a resting order's price-time priority is an asset — sometimes keep the
    quote even though fair value moved, because expected fill quality from the
    front of the queue exceeds the small edge loss from being 1c off. Rough
    rule: only reprice when `|fv − quoted| · fill_prob` exceeds the queue
    value being abandoned; start with "never reprice on a 1-tick move if we're
    near the front."
12b. [ ] **44. Amend + Decrease instead of cancel+replace (M) — elevated to
    L2 above.** Two V2
    endpoints replace the cancel+create pattern (two calls, 3 rate-limit
    tokens, a quote-less window, and post-only-cross risk on re-entry):
    - [Amend Order V2](https://docs.kalshi.com/api-reference/orders/amend-order-v2)
      — change price/size in one call; adopt in the reprice branch.
    - [Decrease Order V2](https://docs.kalshi.com/api-reference/orders/decrease-order-v2)
      (`POST .../orders/{id}/decrease`, `reduce_by`/`reduce_to`) — shrink a
      resting order in place. On price-time exchanges a size-*decrease* is
      exactly the operation that preserves queue priority (it's why a
      dedicated endpoint exists) — which unlocks **queue-preserving inventory
      control**: when flow runs one-sided (run 3: −22 absorbed), decrease the
      exposed side's remaining size instead of choosing between full exposure
      and cancelling away the queue position. Also: trim outer layers (item
      24) without repricing, and give the flow guard a size response, not
      only a price response.
    **Verify semantics first** via gated conformance tests (docs are silent on
    priority): rest an order, amend price → expect priority lost; decrease
    size → expect `remaining_count` correct and priority kept (observable via
    a second session's order behind ours, or at minimum pin the API contract).
    Composes with item 42's queue-value rule: decrease makes "keep the queue,
    cut the risk" a real third option.
13. [x] **41. `spdlog::flush_every(3s)` — PR #52.** *(was:)* `spdlog::flush_every(3s)` (S, ops).** Info-level lines buffer up
    to ~4KB on quiet sessions (flush_on is warn+) — `tail -f` lags minutes
    behind. Found in run 2.
14. [ ] **46. Clock-skew guard (S — found 2026-07-03 evening) — DEFERRED
    (Jacob, 2026-07-04, PR #57 closed unmerged).** The Mac drifted 12m40s and
    every signed request 401'd (`header_timestamp_expired`). Call: keep the
    host clock synced (and move soak runs to the item-37 VM) instead of
    carrying guard code for a host-only failure. Revisit only if drift recurs
    during unattended soak runs; the closed PR #57 branch has a working
    implementation.
15. [x] **47. REST fill backfill on WS reconnect (M — from D10, run 5).**
    *Done — `RestClient::get_fills(min_ts)` (paginated, schema pinned against
    live demo + conformance test), `WebSocketClient::on_reconnect` hook, main
    replays missed fills (60s overlap window) through `session.on_fill`;
    `record_fill` now returns whether the fill was new so duplicates no longer
    reach the flow guard; reconcile clears `kModelDiverge` when back in sync —
    drift halts are recoverable.* Original finding: 3.09 contracts filled
    during a mid-session disconnect were never recorded and the halt was
    permanent.
15a. [ ] **50. D13 — fade-lane oscillator on in-play books (P0 — found run 7,
    2026-07-04). Fix before the next in-play session.** The item-33 fade
    re-opened a bounded churn loop: on the in-play CANMAR book the raw
    micro-price flaps ±3c sub-second, every flap qualifies as an "adverse
    jump", and both sides faded alternately at the fade_rest cadence — 133
    fades / 296 place-cancel pairs in 7 minutes (~42/min; healthy run-3 was
    5–24/min). The rest-time defense works (bounded, no reject storm) but the
    fade lane needs the same discipline: (a) **EMA-smooth the fair-value
    anchor** (item 21(a), now urgent — the root cause is a noisy belief, not
    the gate); (b) require the adverse jump to persist across two consecutive
    updates before fading; (c) optional per-side fade cooldown. Config
    band-aid available today: raise `theo_jump_cents`/`fade_rest_ms` on
    in-play markets.
15b. [ ] **48. Re-quote path independent of book deltas (P1 — found run 6,
    2026-07-04).** Quotes are only placed inside `Quoter::update()`, which
    runs on WS deltas (plus the one startup seed). If the seed placement
    fails (run 6: exchange-wide 503s) — or a side is dropped for any
    transient reason — a market with a quiet book is **quoteless for the
    entire session**: nothing retries. Fix: on the periodic poll cadence
    (e.g., each status tick), for every tracked market with a live book and
    missing resting quotes, call `quoter.update()` with the current
    `LocalOrderbook`. Cheap, idempotent (reprice threshold/rest-time already
    guard churn), and turns any transient placement failure into a ≤60s gap
    instead of a dead session.
15c. [ ] **49. Scanner: liveness filter (P2 — found run 6).** The scanner
    ranks on `vol_24h`, which is stale by up to a day: on a holiday morning
    it picked three markets whose books never ticked once in 8+ minutes
    (golf outrights 14 days from close, `book_age_s=435`). Require recent
    activity — e.g., last trade / book update within N minutes (via
    `/markets/trades` or a short WS sample) — or prefer events starting
    soon. Complements item 36's volume-cap caveat and the D8 scanner
    follow-up.
16. [ ] **45. Decision-oriented quote logging (S).** Today's logs say *what*
    (place/cancel); they should say *why*: per placement log fair value, mid,
    visible inside, inventory + skew, spread components (base/imbalance/fee),
    and the reprice reason. The debug-level `reprice` line has half of this —
    complete it, make it info on actions, and add latency counters
    (quote-generation time, REST RTT, ack latency, WS heartbeat delay) to feed
    the item 37 experiment. (From the external log review.)

**Situational (apply when trading near-close or multi-outcome markets):**

- [ ] **26. √-time terminal size taper (S)** — Γ ∝ 1/√T (Bawa pp.10–11); scale
  size by `√(T_remaining/T_ref)` on TheoGrid's time axis.
- [ ] **27. Closing-day longshot guard (S)** — the closing day is a separate
  regime ("Yogi Berra effect", Bürgi Fig. 8): tighten the price gate <24h to
  close.
- [ ] **28. Quarter-Kelly quote sizing (M)** — edge-scaled size off VAMP fair
  value; gate on item 31's measurements (the <3% figure is a halving-race stat,
  not ruin).
- [ ] **30. Category-conditional, time-decayed debias β (S/M)** — ψ: Crypto
  0.058 (largest), Politics/Entertainment insignificant; 2025 ≈ half the sample
  average (Bürgi Tables 8–9).
- [ ] **34. Sum-to-one monitor + renormalization (M)** — multi-outcome events:
  renormalize fair values by `P_i/ΣP`; alert on executable buy-all arb; defer
  multi-leg execution.
- [ ] **36. Scanner: volume-weight cap + eligibility envelope (S).** Volume
  buys fill probability, not price quality (bias survives every volume
  quintile — Bürgi Tables 6–7; wash-trading caveat — Bawa p.12). *Run-3
  addendum:* the spread tent zeroes at the band edges, so a spread-3 market
  with 3× the volume lost to a spread-4 market — don't zero the tent at the
  configured min spread, and consider depth/trade-frequency over raw vol_24h.

## Production Readiness (after Gate 1 criteria are defined, before real money)

- [ ] **Real-capture replay** — record a demo session into `test/fixtures/`,
  add capture-specific assertions to `replay_session_test`. Also pins the
  `average_fill_price` YES-leg-orientation assumption (PR #45 caveat).
- [ ] **Phase 32 — operational hardening**: service supervision (launchd/systemd
  on the item-37 host), logrotate, Telegram alerts. Required for the long
  unattended demo soak runs Gate 1 needs.
- [ ] **35. Position-accountability guard (S)** — hard per-market cap well
  below Kalshi's 25k/strike limit.
- [ ] **6. Sanitizers in CI (partial)** — ASan job is green on every PR;
  remaining: UBSan flag, a TSan job (2-thread engine is the risk).
- [ ] **7. Tooling guardrails audit** — work through *Use the Tools Available*;
  umbrella over item 6. Detail in archive.
- [ ] **8/D8 scanner follow-up** — consider excluding in-play event markets
  from scanning (pause-prone, event-jump risk) or gating them behind a config
  flag.

## Gated behind Gate 2 — Scaling & Multi-Exchange (do not start)

Detail and diagrams in the archive and [ADR-007](docs/adr/007-process-per-strategy-and-aggregator.md).

| Item | What | Bottleneck it solves |
|---|---|---|
| Phase 21 | Async HTTP order dispatch | REST blocks repricing at ~5 tickers |
| Phase 22 | Per-series WS + thread-per-series | Single WS thread serializes repricing |
| Phase 23 | Incremental RiskManager update | O(n) scan per fill |
| Phase 24 | Aggregator process (PortfolioModel + global risk) | One risk/PnL authority across processes |
| Phase 25 | Cross-ticker delta hedging | Unhedged exposure across series |
| Phase 26+ | Multi-exchange adapters (Polymarket, …) | New venues behind `IHttpTransport`/`IWebSocket` |
| Item 11 | FIX transport | REST order-entry latency — volume-gated, see FIX north-star below |
| Items 15/16 | `Session` concept + process-per-exchange | Multi-exchange isolation |

*Exception:* a Phase-21-style "don't block the WS callback on the rate-limiter
sleep" change may be pulled forward if single-market latency measurements (item
37 experiment) show it matters — it is a quoter-responsiveness issue, not only
a scaling issue.

### North-star: FIX access (Jacob's goal, 2026-07-04)

Kalshi Institutional (Brad, email 2026-07-04): FIX access requires trading
volume **consistently ≥ 5% of total exchange-wide volume on a rolling monthly
basis**. So FIX is a *scale gate*, not a technology unlock — no integration
work shortens the road to it. Consequences:

- **REST + WS is our platform for the foreseeable future.** The low-latency
  package (L0–L4: VM, amend, async dispatch, timer teardown) is not a stopgap
  before FIX — it IS the latency ceiling available to us, which raises its
  priority, not lowers it.
- **The road to FIX runs through the gates in order**: Gate 1 (profitable in
  demo) → Gate 2 (profitable live, then scale markets/uptime) → sustained
  volume growth. Volume per month is the metric that eventually matters;
  track it once live.
- **Intermediate relationship path**: Kalshi's market-maker / liquidity
  incentive programs (item 18 already feeds scanner ranking) are the
  realistic first formal relationship with the exchange; revisit FIX via the
  same institutional thread (reply with volumes + strategy) when scale
  warrants.


## P3 — Structural refactors (nice-to-have, never block the gates)

R1 split `source/` · R2 break up `main.cpp` · R3 `Cents` strong type ·
R4 constraints-vs-guards · R7 message docs + rate-limit review ·
9. queue-position awareness (largely subsumed by item 24). Detail in archive.

## Findings Log

### 2026-07-03 — run 3 (MLB in-play, single market): all five fixes validated live

- **#44 own-quote subtraction**: repricing tracked a moving mid (5–24
  places/min, lifetimes seconds-to-minutes); zero self-oscillation. Run-1
  baseline was 1,146 place/cancel pairs with 98% of orders living <1s.
- **#46 flow-guard floor**: armed at ~13 one-sided contracts (floor =
  quote_size); spread widened 4c → 6c; repricing calmed 24/min → 5/min; seller
  flow paused at the improved prices.
- **#45 flatten PnL**: closed −22.81 @ VWAP 55c (parsed `average_fill_price`;
  the IOC limit was 99c), realized −$0.54 logged and persisted to
  `pnl_state.json` — first session whose true result survived shutdown.
  Post-run reconcile in sync.
- **#47 book_age**: accurate throughout (0–7s on live books).
- **Measurements**: adverse-selection cost of pure passive quoting =
  **−2.4c/contract**; ~6 post-only-cross rejects/10min (D1 residual → items
  3/37); info-log buffering on quiet sessions (→ item 41).
- **External log review (LLM, 2026-07-03)** corroborated the priority order
  (inventory-aware quoting, churn hysteresis, fill analytics, decision
  logging — items 25/32, 42, 31, 45) and caught a real pricing anomaly the
  monitors missed: isolated `yes@28/29` and `bid 55` placements amid 46–52
  quoting → crossed/degenerate visible-book flicker → **item 43** (sanity
  guard). Quoter hot path benchmarked and ~2× faster (PR #51):
  256–315 ns/update steady-state.

### 2026-07-04 — run 7 (wave-2 quoter live): first Gate-1 metrics; D13 found

7 min live on 5 markets (in-play Canada–Morocco + hot dog contest + 3 WC
markets), first session with fade + LMSR skew + longshot floor + analytics:

- **First measured markout (the Gate 1 metric): +1.35c/contract @30s, −0.46c
  @5min** (11 maker fills, one market) vs. the run-3 baseline of
  **−2.4c/contract**. Effective spread 4.06c. Small sample, but the @5min
  number already sits inside the proposed Gate-1 PASS bound (≥ −0.5c).
- Session PnL **−$0.23** with a clean SIGINT flatten (short 12.28 bought back
  @28) and reconcile **in sync** throughout — in-play exposure whose damage
  was bounded by the skew walking the ask up as the short built.
- **D13 (item 50): fade-lane oscillator** — 133 theo fades, all on the
  in-play book, both sides alternating at ~600ms; raw micro-price noise
  passes the theo-jump gate every fade window. Churn 42 place/min (bounded,
  zero failed cancels, but burns write budget and queue priority).
- Item 31b pipeline validated end-to-end live: analytics JSONL → markout
  report worked first try.
- Ops: 11 transient 503s mid-session (self-recovered), 4 post-only crosses
  (D1 residual), flow guard never armed (fills were slow drips, one-sided
  ratio under window threshold).

### 2026-07-04 — run 6 (first run of the wave-2 quoter): demo order placement down

- Attempted the first live validation of theo-fade + LMSR skew + longshot
  floor (items 33/25/29). **No quoting data: every order create returned
  `HTTP 503 service_unavailable (service: exchange)`** — on all 3 scanned
  markets, across a restart, and confirmed exchange-wide by the conformance
  suite's own market pick (`CreateOrderResponseParsesRestsAndCancels` 503s).
  Reads, WS snapshots/deltas, and `/exchange/status` (`trading_active:
  true`!) all healthy — demo order entry was down on the July 4 holiday.
  Re-run the 20-minute validation session when placement recovers.
- **D11 → item 48:** failed seed + quiet book = quoteless session (no retry
  path outside WS deltas).
- **D12 → item 49:** scanner picked dormant markets (vol_24h is yesterday's
  number; holiday morning books never ticked).
- Ops notes: Mac clock synced (+0.09s, `sntp`); first informal L0 datapoint —
  unauthenticated `GET /markets` from the Mac ≈ **140ms**; status-endpoint
  `trading_active` does NOT imply order entry is up (a placement probe is the
  only honest preflight).

### 2026-07-03 evening — runs 4–5 (integration preview: PRs #51-#54 + #52)

- **Run 4** (single market, 15 min, "7+ corners"): $0.00, zero fills — and
  zero churn/rejects/warnings across 15 min of live deltas (pre-fix
  equivalent: ~1,700 orders). Discipline confirmed; no edge data.
- **Run 5** (5 markets, ~13 min): **−$2.59** (CAPT −$0.39; MLB WSH −$2.20 =
  −22c/contract — sold into an in-play team-win move 66c → 91c). New findings:
  - **D9 — echo-race oscillator (item 42 now urgent):** ARG-CAPT flipped
    between two quote states 3c apart every ~150ms (376 places). The D4 fix
    subtracts our *current* quotes, but after cancel@27/replace@30 the book
    still echoes the *old* 27-level for a beat — self-reference in time, not
    space. Only rest-time hysteresis (item 42) kills the whole class.
  - **D10 — disconnect fills lost → drift halt (item 47):** WS dropped
    mid-churn (server guard suspected); 3.09 contracts filled in the gap were
    never seen; reconcile caught it (local −22.61 vs exchange −25.70) and
    halted via `kModelDiverge`. Safety stack validated end-to-end: cancel-all
    on disconnect, drift detection, halt, flatten at **exchange** truth, PnL
    recorded and persisted for both tickers.
  - **Clock-skew outage (item 46):** 12m40s local drift 401'd every signed
    request until resynced.
- Lifetime demo ledger (honest, carried): **−$3.13**.

### 2026-07-03 — run 1 findings D4–D8: all resolved same day

D4 self-referential micro-price oscillator → PR #44 · D5 flow defenses
(floor half → PR #46; skew half → item 25) · D7 flatten PnL lost → PR #45 ·
D8 idle-vs-wedged → PR #47 (+ scanner follow-up above). Full evidence in the
archive.

### Earlier history

2026-06-29 demo findings (D1–D3), the 2026-07-03 bug audit A1–A7 (all fixed,
PRs #27–#39), phases 1–31, UAT blockers (all resolved — demo conformance
passes 10/10), and the full Done log: see the
[archive](docs/archive/PLAN-2026-07-03-full.md).

## Working Agreements (unchanged)

- TDD, one PR per fix, review-order section in every PR description
  (CLAUDE.md).
- Demo creds: `config-demo.json` (gitignored); source of truth in
  `/Users/jacobfreund/kalshi-demo-key/`.
- Every new component gets a Mermaid diagram — current diagrams live in
  [docs/architecture.md](docs/architecture.md); superseded phase diagrams are
  in the archive.
