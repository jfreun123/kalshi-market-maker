# Pre-Live Checklist — Gate 1 Definition

Status: **DRAFT — proposed numbers, Jacob must ratify or edit every value in
§2 before this becomes the binding Gate 1 criteria** (PLAN item 19; Strategy
Gates in PLAN.md). Nothing goes to production until §2 reads PASS on real demo
data, evaluated by the item-31 analytics — not by eyeball.

## 1. The edge, in one falsifiable sentence

> Resting two-sided post-only quotes on mid-range (15–85c) Kalshi markets,
> anchored on a micro-price that reprices faster than the retail taker flow
> that hits us, earns spread capture plus liquidity-incentive income that
> exceeds adverse-selection losses plus fees, measured over any rolling
> two-week demo window.

Who pays us: retail takers crossing the spread for immediacy, and Kalshi's
liquidity-incentive pools. Why competitors haven't removed it: mid-range
Kalshi markets are small enough that professional MMs under-serve them
(Bürgi §6: top-decile event volume ≈ $526k). How it dies: if measured markout
shows takers are systematically informed (run 3: −2.4c/contract before the
strategy items land), the sentence is false and the strategy must change —
not the threshold.

## 2. Gate 1 thresholds (PROPOSED — ratify or edit)

Evaluated over a rolling window of **14 demo sessions totalling ≥ 30 hours**,
single market per session, all sessions included (no cherry-picking):

| Criterion | PASS | FAIL (iterate) | KILL (stop, rethink) |
|---|---|---|---|
| Net PnL (realized + final MTM, incl. fees) | > $0 | ≤ $0 | < −$25 cumulative |
| Avg markout @5min per filled contract | ≥ −0.5c | < −0.5c | < −3c persistent |
| Sample size | ≥ 300 contracts round-tripped | insufficient → extend window | — |
| Per-session loss | — | — | < −$10 twice in the window |
| Reconcile | in sync at every session end | — | any unexplained drift |
| Behavior | no churn storms, no crossed-book placements, no halts except by design | — | any uncontrolled behavior recurring after a fix |

PASS on all rows → Gate 1 satisfied → smallest possible live deployment
($50-class bankroll, one market, same thresholds re-evaluated live).
FAIL → work the PLAN "Now" list; re-run the window. KILL → the edge sentence
is falsified; revisit strategy, not parameters.

## 3. Measurement integrity rules

- Markout is the evaluation metric only — it must never double as a fill
  trigger or quoting input while also serving as the judge (practitioner
  checklist).
- Thresholds are frozen *before* a window starts; mid-window edits void the
  window.
- Demo→live divergence: for the first live window, record fill rate, queue
  position at fill, and markout side-by-side against the trailing demo window;
  a live fill-rate or markout materially worse than demo (proposed: >2×
  worse markout) reverts to demo until explained.

## 4. Prerequisites before a window can start

- [ ] Item 31 analytics landed (per-fill markout, effective spread, PnL
      attribution) — without it §2 is unmeasurable.
- [ ] Item 43 sanity guard + item 42 hysteresis landed (behavior row).
- [ ] Phase 32 minimum: unattended session supervision + alerts, so 30 hours
      is practical.
- [ ] `pnl_state.json` carry verified across restarts (shipped, PR #45).

## 5. Already satisfied (validated live 2026-07-03)

Disconnect/stale-book handling · cancel-on-exit + orphan cancel-on-start ·
verified kill-switch & flatten (exit ends flat, reconcile in sync) ·
post-only-only quoting · fill dedup by trade_id · carry persistence ·
demo conformance suite 10/10.
