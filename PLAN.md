# Kalshi Market Maker — Build Plan

> Lean plan, updated 2026-07-05. Full history (all completed items, resolved
> findings D1–D14, runs 1–7 detail):
> [docs/archive/PLAN-2026-07-04-full.md](docs/archive/PLAN-2026-07-04-full.md)
> and [docs/archive/PLAN-2026-07-03-full.md](docs/archive/PLAN-2026-07-03-full.md).
> Research: [docs/papers](docs/papers/README.md) · architecture:
> [docs/architecture.md](docs/architecture.md) · guards catalog:
> [docs/GUARDS.md](docs/GUARDS.md) · API ground truth:
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

1. [x] **Validate the D13/D14 fixes live.** *PASSED — run 12 (2026-07-04
   21:15, 5 min, self-selected in-play MLB PHI@KC): **0 fades, 0 fade-pends,
   ~1.3 order-moves/min** (vs 133 fades / 42 moves/min in run 7 pre-fix) on a
   live in-play book; 3 maker fills, markout@30s 0.00c (n=3, within the
   ≥−0.5c bound); repricing tracked the game via 4 atomic amends (L2 live);
   clean flatten −$0.03. The startup market self-selection picked the live
   game unaided.*
2. [x] **L0 — latency baseline.** *Done — per-request RTT flows into the
   analytics JSONL (`type:"http"`); `scripts/latency_report.py` prints
   p50/p95/max per endpoint. Mac baseline (2026-07-04): order-path ~294ms,
   orderbook ~76ms, general GETs p50 184ms — the numbers L1 must beat.*
3. [ ] **L1 — EC2 in Kalshi's region** (t4g.small ~$12/mo; NOT a trading-VPS
   product). Probe us-east-1 vs us-east-2 RTT first; rerun the L0 session
   from the winner; fold host into Phase 32 supervision.
4. [x] **L2 = item 44 — Amend + Decrease Order V2.** *Done — semantics
   pinned live (amend = POST .../orders/{id}/amend with a full order body,
   response may carry a new order_id per official docs, demo returns the
   same; decrease = .../decrease keeps the id = keeps priority). The quoter
   reprice branch now issues ONE atomic amend instead of cancel+replace
   (fallback to cancel+replace on amend rejection; self-cross still cancels).
   Halves reprice round-trips and removes the quote-less window — the D9
   echo root. `decrease_order` is available for the flow guard's future
   size response (item 32/24).* Original scope: One atomic call replaces
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
11. [x] **32 — directional flow lean + inventory brake.** *Done — while the
    flow guard is imbalanced, fair value leans `flow_lean_cents` (1c) toward
    the takers' side (believe persistent flow; decays with the guard's 5-min
    window); and at `inventory_cap_lots` × quote_size (2× = 20) the
    accumulating side stops quoting entirely — the run-13 pattern (30-lot
    pile-up into a trend, drift −$0.42 + exit −$1.05) is now capped at 20
    with the belief already leaning toward the move. Validated — run 14
    A/B on run 13's market: −$0.78 → −$0.20, peak inventory 30 → 10, drift
    −$0.42 → −$0.10, exit −$1.05 → −$0.35, entry edge stayed positive
    (+2.5c/lot).* Original scope: signed IR → bounded offset (Bawa p.8).
12. [ ] **24 — layered quoting (queue priming)**: 2–3 size layers 1–3c behind
    the inside; queue position is earned by resting before the move.
13. [ ] **42b — queue-value reprice rule**: only reprice when
    `|fv − quoted| · fill_prob` beats the queue value abandoned (needs 31
    fill-prob data).
14. [x] **53 — order janitorial.** *Done — startup cancels every resting
    order account-wide; shutdown sweeps `get_open_orders()` with retries
    until the exchange confirms clean. Validated live same day.*
15. [x] **49 — scanner liveness filter.** *Done — candidates whose last
    public trade is older than `max_stale_trade_minutes` (30) are dropped;
    validated live (picked the one genuinely trading market).*
16. [x] **52 — in-session market rotation.** *Done — every
    `rotation_minutes` (5) the session re-scans, drops dead+idle markets
    (never with a position or resting orders), adopts live picks.
    Follow-up 2026-07-04: **market selection moved into startup** — the
    session scans for its own tickers when `target_tickers` is empty (now
    the default); `--scan` is research-only; the generated
    `config-demo.trade.json` two-process workflow and `write_trade_config`
    were removed; `run_demo.sh` is single-process.*
17. [ ] **45 — decision-oriented quote logging** (the "why" per placement:
    spread components, reprice reason; latency counters fold into L0).
18. [ ] **23 — ceil-per-order maker-fee model** (mostly inert while demo maker
    fills are free); **3 — passive clamp vs fresher BBO** (D1 residual;
    largely solved by L1+L2).

19. [ ] **54 — batch CreateOrders (Jacob, 2026-07-04).** Kalshi V2 supports
    batch create/cancel (token cost per item, batch size scales with tier).
    Batch the session's order placements — seeds and layered quotes (item
    24) especially — into one request: fewer round trips, coherent quote
    placement. Sequence after L3 (async dispatch) so batching composes with
    the non-blocking order path.

20. [ ] **55 — demo conformance suite in CI (Jacob, 2026-07-04).** Run the
    live `demo_conformance_test` in CI (nightly schedule + manual dispatch,
    not per-PR): needs demo creds as GitHub Actions secrets (`KALSHI_DEMO_
    API_KEY` + PEM written to a runner temp file, `KALSHI_DEMO_CONFIG`
    generated in the job). Non-required job — it places real demo orders; a
    red nightly means schema drift or demo outage, both worth an alert.
    Skips are failures (Jacob): tests self-find a market (scanner pick →
    any open market → FAIL); only missing creds may skip. Local status
    2026-07-04: 12/12 pass live, ~5 min.

21. [x] **56 — passive wind-down before session end (found by run-12
    attribution).** *Done — on shutdown the quoter goes reduce-only (only the
    inventory-reducing side quotes; the accumulating side is cancelled; flat
    markets place nothing) and works the position out as a MAKER for up to
    `quoter.winddown_seconds` (45; 0 = old behavior) while the feed stays
    live; only the remainder is taker-flattened. Turns the exit from paying
    the spread into earning it.* Original finding: The session-end flatten is a TAKER order: it pays the
    spread plus any drift, and on short sessions it systematically gives
    back the maker edge (run 12: entries earned +$0.015, the flatten gave
    back ~−$0.045 → net −$0.03; zero pick-offs, zero holding drift — the
    exit was the entire loss). Fix: in the final N minutes, stop opening and
    quote-out inventory passively (one-sided maker quotes at/inside the
    reservation); flatten only what remains at the bell. Longer sessions
    dilute the same fixed cost.
22. [~] **31c — PnL attribution: shipped `scripts/pnl_attribution.py`**
    (entry_edge / drift / exit_cost split + per-fill quote-age and pre-fill
    belief drift = the picked-off / latency-loss detector). record_flatten
    now emits analytics fill events so exits are measurable. Remaining:
    fees term once maker-fee markets are traded; 31a Brier join unchanged.

23. [x] **57 — skew-per-fill cap (Jacob's catch, 2026-07-04).** *Done — with
    b anchored only to max_position (~45.5), ONE quote-sized fill shifted the
    reservation ~5.5c past the 2c half-spread: buy 50 → offer 49, a locked-in
    loss on every calm round trip. b is now floored at 25×quote_size (=250),
    holding a single-fill shift to ~1c at mid: buy 50 → offer 53, next bid 49
    (the gradual directional widening) — skew biases flow, never quotes a
    guaranteed loss. Trade-off: at max_position the reservation reaches
    ~±10c, not the band edge; the position cap remains the hard stop.
    Regression: SingleFillNeverQuotesAGuaranteedLossExit.*

24. [ ] **58 — scanner startup efficiency (external review, 2026-07-04).**
    Startup scans ~50k markets for ~28s before quoting. Query
    `status=open` server-side, cap pagination, and/or cache market metadata
    between runs with incremental refresh. Not latency-critical yet;
    becomes waste on the VM and on every rotation re-scan.
25. [ ] **59 — short-horizon markout (external review).** Add 500ms / 1s /
    5s horizons to `analyze_fills.py` alongside 30s/5min — the fast
    horizons separate "quoting too aggressive" (instant reversion) from
    genuine adverse selection (slow drift). Also aggregate edge by
    inventory level and add avg-entry/mark/edge to the per-fill status log
    (folds into item 45).

26. [ ] **60 — regression calibration (Jacob, 2026-07-04).** Two tiers:
    (a) **Drift estimator — build first, needs no fill history**:
    significance-gated rolling regression (slope + t-stat) of the smoothed
    mid over the last N minutes, fit on the quote-event stream we already
    log every tick. Fires on structural trends (mention-market staircase),
    silent on random-walk chop. Uses, both defensive: scale the flow lean
    (`lean = clamp(k·slope·horizon, ±max)`, replacing the fixed 1c —
    Avellaneda-Stoikov with a drift term) and **penalize |slope| in the
    scanner** — a significantly trending book is where makers bleed
    (run 14's residual −$0.20); prefer two-sided chop, where spread pays.
    (b) **Fill-history models — gated on ~500 accumulated fills**: toxicity
    regression (post-fill markout on flow ratio, book imbalance, inventory,
    spread → per-fill required-edge floor, replacing literature constants);
    learned micro-price (future mid on mid/micro/imbalance blend); logistic
    fill-probability (unblocks 42b queue-value and 28 Kelly sizing). Every
    soak session's analytics JSONL is the training set.

**Selection principle (Jacob, 2026-07-04): profitable on every market we
CHOOSE, then scale.** Not every market can be made profitably — trending
books, dead books, and 1c-spread deep books all bleed makers. Scaling (Gate
2) multiplies per-market results, so the bar is: only quote markets where
measured expected edge is positive (liveness ✓, spread band ✓, drift penalty
= item 60a, toxicity floor = 60b), and exit any market whose live attribution
turns negative (rotation already provides the mechanism).

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

## State of play (2026-07-05)

- **Quoter** (full catalog + provenance: [docs/GUARDS.md](docs/GUARDS.md)):
  EMA fv + min-rest + adverse fade + LMSR skew (25×quote_size floor) +
  longshot floor + flow lean + inventory brake + wind-down + maker-favor
  rounding + crossed-book guard + own-quote subtraction. Validated: run 12
  in-play (0 fades, atomic amends); run 14 A/B vs run 13 on the same
  trending market: −$0.78 → −$0.20, peak inventory 30 → 10.
- **Loss taxonomy settled (runs 13/14)**: per-fill pricing earns (+0.5–2.5c
  entry edge); latency is a non-issue at demo speeds (1/45 picked off);
  exit machinery built. The remaining leak is quoting structurally trending
  markets — attacked by item 60a (drift estimator) + the selection
  principle.
- **Flow**: startup scans and selects its own live markets (top
  `trade_top_n`, default 1); liveness-filtered; rotated every 5 min;
  account-wide order hygiene at both ends of the session. Run 15
  (2026-07-05, first on fully-merged main): clean but zero fills on a quiet
  Saturday-morning book; scanner again picked a mention-family (trending)
  market — 60a's scanner penalty is the fix.
- **Measurement**: analytics JSONL (quotes, fills, http RTT) →
  `analyze_fills.py` (markout) + `pnl_attribution.py` (entry/drift/exit +
  picked-off) + `latency_report.py` (L0 baseline above).
- **Codebase**: cleanup pass 1+2 done — dead modules removed
  (`adverse_selection`, `write_trade_config`), `main.cpp` 810→~520 lines
  (mode runners in `app_modes.{hpp,cpp}`, tested), 1,233-line quoter test
  split into pricing + reprice suites. Suite 456 tests green.
- **Demo carry ledger −$4.38** (`pnl_state.json`, 2026-07-05). PnL needs
  fills-in-volume: run sessions during live slates; the machinery debate is
  settled.
- Demo quirks: order entry can 503 exchange-wide while `/exchange/status`
  says active; fills can be fractional; laptop sleep mid-session is safe but
  wastes the session — soak runs belong on the L1 VM.

## External review reconciliation (2026-07-04, run-13 log)

An independent log review rated run 13 **8/10** ("behaving like a passive
liquidity provider; next phase is inventory management and measuring edge").
Reconciliation: its top findings — inventory grows unbounded, no reduction
logic, no realized spread capture — were fixed the same night (#85 wind-down,
#86 skew-per-fill cap, #87 flow lean + 2×-quote-size inventory brake), and
its "missing metrics" largely exist in the analytics JSONL + attribution/
markout scripts (invisible to a log-only review; item 45 will surface them
in the human log). Adopted as new items: 58 (startup scan efficiency), 59
(short-horizon markout + edge-by-inventory). Its "NO bid too aggressive"
hypothesis is contradicted by measured entry edge (+0.5–2c per fill,
1/35 picked off) — the leak was accumulation, not per-fill pricing.

## Working agreements

TDD; one PR per fix; PR descriptions start with a Review-order section;
**no stacked PR chains — sequential PRs on main** (stacked merges landed in
base branches instead of main on 2026-07-04 and had to be re-landed);
commits ≤15 words, no attribution; secrets live in
`/Users/jacobfreund/kalshi-demo-key/`, never in git; Mermaid diagrams in
docs/architecture.md.
