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

1. [ ] **19. Falsifiable edge + PASS/FAIL/KILL thresholds.** State the edge in
   one falsifiable sentence; pre-declare the demo-profitability criteria that
   Gate 1 is judged by; plan live-vs-paper fill-divergence measurement. Output:
   `docs/PRE_LIVE_CHECKLIST.md`. *This defines what "profitable in demo" means —
   do it first.*
2. [ ] **31. Measurement backbone: calibration + fill analytics (S/M).**
   (a) Brier logger: persist `(ticker, t, P_fair, P_market, config flags)` per
   quote decision, join settlement outcomes, score offline (Bawa p.9/11).
   (b) Per-fill quality: log fair value, mid, expected edge, and inventory at
   fill time; compute markout (mark at +30s/+5min) and effective spread
   offline — every fill should answer "was this a good fill, or adverse
   selection?" (c) PnL attribution split: realized spread vs. mark-to-market
   vs. inventory vs. fees (extends `exposure_decomposition`). Required for
   Gate 1 and for tuning every knob below.
3. [x] **22. Round quotes in the maker's favor (S).** *Done — merged PR #53:
   `floor` bid, `ceil` ask.* (Berg & Proebsting pp.53–54.)
4. [ ] **23. Maker-fee widening ON by default + ceil-per-order fee model (S).**
   The documented maker edge predates maker fees (Bürgi p.6); fee rounds up to
   the cent per order (1.77% effective at 50c×100).
5. [ ] **33. Cancel-on-theo-jump quote fade (M).** The maker edge *is*
   repricing (Bürgi p.27): if VAMP/theo moves > k cents against a resting
   order, cancel out-of-cycle instead of waiting for the next tick. Directly
   attacks the −2.4c/contract measured in run 3.
6. [x] **43. Visible-book sanity guard (S — found by external log review).**
   *Done — merged PR #54: update skipped (resting quotes kept) when the visible
   book is crossed (`best_bid ≥ best_ask`), with a warn log.* Original run-3
   evidence: `yes@28`, `yes@29`, bid `55` placed in a market quoting 46–52
   during fast sweeps. Residual (not shipped): implausible-inside-jump check
   vs. the last mid.
7. [ ] **32. Directional flow lean (M).** IR > 0.65 → bounded fair-value offset
   (±1c) or asymmetric size, decaying over 15–30 min (Bawa p.8). Validate
   thresholds via item 31 markout — the headline R² is an equities number.
8. [ ] **29. Asymmetric quoting — longshot-side edge floor (M).** Maker returns
   are negative on all sub-50c buys (Bürgi Fig. 6); require extra edge or less
   size on the cheap-side bid.
9. [ ] **25. LMSR log-odds inventory skew (S).** Replace linear
   `skew_per_contract_cents` with `fv' = c/(1+(c/fv−1)·e^{q/b})`; derive `b`
   from the risk budget (hit `max_position` ⇒ reservation price reaches
   P_upper). Fixes the invisible-skew half of D5. (Berg & Proebsting pp.49–56.)
10. [ ] **24. Layered quoting — queue priming (S/M).** Rest 2–3 size layers 1–3c
   behind the inside: price-time FIFO means queue position is earned by resting
   *before* the move (item 9's measurement: ~115k ahead when joining late);
   sweeps pay progressively more; each layer earns incentive score. Genuine
   intent-to-fill laddering — avoid the word "layering" externally (regulators
   use it for the manipulative variant). Depends on own-quote subtraction
   (shipped, PR #44).
11. [ ] **3. Passive clamp vs. fresher BBO (D1 residual, M) + 37. deploy near
    the matching engine (S, ops).** Run 3 measured ~6 `post only cross`
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
12b. [ ] **44. Amend + Decrease instead of cancel+replace (M).** Two V2
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
14. [x] **46. Clock-skew guard (S — found 2026-07-03 evening).** *Done —
    `clock_skew.{hpp,cpp}` parses the server `Date` header;
    `RestClient::measure_clock_skew` (works even on 401 responses); startup
    refuses to trade beyond 5s skew and the reconcile cadence sets/clears a
    new `kClockSkew` halt, so a mid-session drift halts quoting and recovery
    is automatic once the clock resyncs.* Original finding: the Mac drifted
    12m40s and every signed request 401'd (`header_timestamp_expired`).
15. [x] **47. REST fill backfill on WS reconnect (M — from D10, run 5).**
    *Done — `RestClient::get_fills(min_ts)` (paginated, schema pinned against
    live demo + conformance test), `WebSocketClient::on_reconnect` hook, main
    replays missed fills (60s overlap window) through `session.on_fill`;
    `record_fill` now returns whether the fill was new so duplicates no longer
    reach the flow guard; reconcile clears `kModelDiverge` when back in sync —
    drift halts are recoverable.* Original finding: 3.09 contracts filled
    during a mid-session disconnect were never recorded and the halt was
    permanent.
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
| Item 11 | FIX transport | REST order-entry latency — evaluate only after item 37 |
| Items 15/16 | `Session` concept + process-per-exchange | Multi-exchange isolation |

*Exception:* a Phase-21-style "don't block the WS callback on the rate-limiter
sleep" change may be pulled forward if single-market latency measurements (item
37 experiment) show it matters — it is a quoter-responsiveness issue, not only
a scaling issue.

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
