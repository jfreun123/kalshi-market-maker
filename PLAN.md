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
2. [ ] **31. Brier/markout calibration logger (S).** Persist
   `(ticker, t, P_fair, P_market, config flags)` per quote decision; join
   settlement outcomes; score offline. The measurement backbone for every knob
   below and for Gate 1 itself. (Bawa p.9/11.)
3. [ ] **22. Round quotes in the maker's favor (S).** `compute_quotes` uses
   `std::round` both sides — up to 0.5c/fill giveaway. `floor` bid, `ceil` ask.
   (Berg & Proebsting pp.53–54.)
4. [ ] **23. Maker-fee widening ON by default + ceil-per-order fee model (S).**
   The documented maker edge predates maker fees (Bürgi p.6); fee rounds up to
   the cent per order (1.77% effective at 50c×100).
5. [ ] **33. Cancel-on-theo-jump quote fade (M).** The maker edge *is*
   repricing (Bürgi p.27): if VAMP/theo moves > k cents against a resting
   order, cancel out-of-cycle instead of waiting for the next tick. Directly
   attacks the −2.4c/contract measured in run 3.
6. [ ] **32. Directional flow lean (M).** IR > 0.65 → bounded fair-value offset
   (±1c) or asymmetric size, decaying over 15–30 min (Bawa p.8). Validate
   thresholds via item 31 markout — the headline R² is an equities number.
7. [ ] **29. Asymmetric quoting — longshot-side edge floor (M).** Maker returns
   are negative on all sub-50c buys (Bürgi Fig. 6); require extra edge or less
   size on the cheap-side bid.
8. [ ] **25. LMSR log-odds inventory skew (S).** Replace linear
   `skew_per_contract_cents` with `fv' = c/(1+(c/fv−1)·e^{q/b})`; derive `b`
   from the risk budget (hit `max_position` ⇒ reservation price reaches
   P_upper). Fixes the invisible-skew half of D5. (Berg & Proebsting pp.49–56.)
9. [ ] **24. Layered quoting — queue priming (S/M).** Rest 2–3 size layers 1–3c
   behind the inside: price-time FIFO means queue position is earned by resting
   *before* the move (item 9's measurement: ~115k ahead when joining late);
   sweeps pay progressively more; each layer earns incentive score. Genuine
   intent-to-fill laddering — avoid the word "layering" externally (regulators
   use it for the manipulative variant). Depends on own-quote subtraction
   (shipped, PR #44).
10. [ ] **3. Passive clamp vs. fresher BBO (D1 residual, M) + 37. deploy near
    the matching engine (S, ops).** Run 3 measured ~6 `post only cross`
    rejects/10min on an in-play book — the ~300ms local RTT is the cause. Item
    37 (a US-East VM: measure RTT → demo session → compare reject rates) likely
    buys more than any software mitigation and gates whether FIX (item 11) is
    ever worth it.
11. [ ] **42. Minimum quote rest time / two-tick reprice hysteresis (S).**
    Defense-in-depth vs. oscillation sources other than self-feedback; also
    keeps behavior unambiguously far from manipulative-cancel patterns
    (see item 24 note). Follow-up to item 38 (shipped).
12. [ ] **41. `spdlog::flush_every(3s)` (S, ops).** Info-level lines buffer up
    to ~4KB on quiet sessions (flush_on is warn+) — `tail -f` lags minutes
    behind. Found in run 2.

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
