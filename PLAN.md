# Kalshi Market Maker — Build Plan

> Lean plan, updated 2026-07-04. Full history (all completed items, resolved
> findings D1–D14, runs 1–7 detail):
> [docs/archive/PLAN-2026-07-04-full.md](docs/archive/PLAN-2026-07-04-full.md)
> and [docs/archive/PLAN-2026-07-03-full.md](docs/archive/PLAN-2026-07-03-full.md).
> Research: [docs/papers](docs/papers/README.md) · architecture:
> [docs/architecture.md](docs/architecture.md) · API ground truth:
> [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md).

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

## Now (ordered)

1. [~] **Validate the D13/D14 fixes live** — needs an in-play book. Run 8
   (2026-07-04 14:21, ~7 active min, 4 markets): 8 seeds placed cleanly, then
   **zero cancels / zero fades / zero fills** — books were calm, so this
   proves no false-positive fades but not behavior under in-play fire; no
   markout data. Bonus resilience datapoint: the Mac slept mid-session
   (29-min log gap); on wake the bot cancelled all 8 quotes, flattened, and
   reconcile is in sync — a laptop sleep is now a safe failure. PASS criteria
   for the real test: single-digit fades, ≤24 order-moves/min, markout
   ≥ +1.35c@30s (`scripts/analyze_fills.py logs/analytics_*.jsonl`).
2. [ ] **L0 — latency baseline before any infra change.** Surface per-request
   RTT (already timed at debug in `http_transport.cpp`), ack latency, and
   delta→decision→request gaps into the analytics JSONL; p50/p95/max report;
   one baseline session on the Mac, numbers recorded here.
3. [ ] **L1 — EC2 in Kalshi's region** (t4g.small ~$12/mo; NOT a trading-VPS
   product). Probe us-east-1 vs us-east-2 RTT first; rerun the L0 session
   from the winner; fold host into Phase 32 supervision.
4. [ ] **L2 = item 44 — Amend + Decrease Order V2.** One atomic call replaces
   cancel+replace (no quote-less window, likely no book echo = the D9 root).
   Pin semantics first via demo-conformance tests: amend price → priority
   lost, no dead-level echo; decrease size → priority kept. Decrease gives
   queue-preserving inventory control.
5. [ ] **L3 — async order dispatch** (sanctioned Phase-21 pull-forward): order
   REST calls block the WS thread today; the bot is deaf while one is in
   flight.
6. [ ] **L4 — timer teardown.** After L2 proves echo-free AND item-1
   validation holds: shrink `min_rest_ms`/`fade_rest_ms` toward zero
   (config-only). Governors that remain: reprice threshold, write budget, RTT.
7. [ ] **19 — Jacob ratifies Gate-1 thresholds** (PRE_LIVE_CHECKLIST §2);
   the 14-session/30h measurement window can't start until frozen.
8. [ ] **Phase 32 minimum — unattended supervision** (launchd/systemd on the
   L1 host, logrotate, Telegram alert on halt/error) — required for the 30h
   soak.
9. [ ] **31a/31c — finish the measurement backbone**: settlement join + Brier
   scoring (quote stream already persisted); PnL attribution split (spread vs
   mark-to-market vs inventory vs fees).
10. [ ] **51 — panic pull tier (Jacob to confirm)**: on a catastrophic jump
    (≥ `panic_jump_cents`, e.g. 8), cancel the toxic side instantly — no
    confirmation, no rest floor, no re-quote until the book settles.
11. [ ] **32 — directional flow lean**: signed IR signal → bounded fv offset
    (±1c), decaying 15–30 min; validate thresholds on item-31 markout.
12. [ ] **24 — layered quoting (queue priming)**: 2–3 size layers 1–3c behind
    the inside; queue position is earned by resting before the move.
13. [ ] **42b — queue-value reprice rule**: only reprice when
    `|fv − quoted| · fill_prob` beats the queue value abandoned (needs 31
    fill-prob data).
14. [ ] **49 — scanner liveness filter**: vol_24h is stale; require recent
    trades/book activity so holiday-morning dormant markets don't get picked.
15. [ ] **45 — decision-oriented quote logging** (the "why" per placement:
    spread components, reprice reason; latency counters fold into L0).
16. [ ] **23 — ceil-per-order maker-fee model** (mostly inert while demo maker
    fills are free); **3 — passive clamp vs fresher BBO** (D1 residual;
    largely solved by L1+L2).

**Situational** (apply when relevant): 26 √-time size taper · 27 closing-day
longshot guard · 28 quarter-Kelly sizing (gate on 31) · 30 per-category debias
β · 34 sum-to-one monitor · 36 scanner volume-cap.

**Production readiness** (after Gate-1 definition): real-capture replay
fixture · 35 per-market position cap · 6 UBSan/TSan CI jobs · 7 tooling audit
· scanner in-play config flag (D8 follow-up).

## Gated behind Gate 2 (do not start)

Phase 21–26 scaling architecture, multi-exchange, FIX transport (see
north-star), `Session` concept — detail in the archive and
[ADR-007](docs/adr/007-process-per-strategy-and-aggregator.md). P3 structural
refactors (R1–R4, R7) never block the gates.

## State of play (2026-07-04)

- **Quoter stack shipped and merged**: min-rest hysteresis + adverse
  theo-jump fade (two-tick confirmed) + EMA-smoothed fair value (α=0.2) +
  LMSR log-odds inventory skew (b from risk budget) + longshot edge floor
  (<40c buys +1c) + maker-favor rounding + crossed-book guard + own-quote
  subtraction.
- **Resilience**: REST fill backfill on WS reconnect, recoverable drift
  halts, idle-market re-quote (30s), orphan cancel on start, clean SIGINT
  flatten, PnL carry, fill dedup.
- **Measurement**: analytics JSONL (quote decisions + fills) →
  `scripts/analyze_fills.py` markout/effective-spread report. Working live.
- **Run 7** (first wave-2 session, 7 min in-play): markout **+1.35c@30s /
  −0.46c@5min** (n=11) vs −2.4c run-3 baseline; PnL −$0.23; found D13 fade
  oscillator + D14 rest-clock bug → both fixed (PR #69), not yet validated
  live (item 1).
- **Lifetime demo ledger: −$3.36.**
- Demo quirks: order entry can 503 exchange-wide while `/exchange/status`
  says `trading_active` (July 4 outage); fills can be fractional contracts.

## Working agreements

TDD; one PR per fix; PR descriptions start with a Review-order section;
**no stacked PR chains — sequential PRs on main** (stacked merges landed in
base branches instead of main on 2026-07-04 and had to be re-landed);
commits ≤15 words, no attribution; secrets live in
`/Users/jacobfreund/kalshi-demo-key/`, never in git; Mermaid diagrams in
docs/architecture.md.
