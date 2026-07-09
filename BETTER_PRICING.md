# Better Pricing — the clearing price as fair value

> Design doc for PLAN items 66 (clearing-price fair value) and 65
> (two-sided-flow admission), plus the knob consolidation both unlock.
> Companion to [PLAN.md](PLAN.md) · guards catalog:
> [docs/GUARDS.md](docs/GUARDS.md) · API ground truth:
> [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md).

## 1. The principle

An HFT interview question (Jacob's): *market participants share limit orders
that may cross. Given all orders at once, find the single fair price.* The
answer: **the price that maximizes matched volume** — where cumulative demand
meets cumulative supply and the most back-and-forth flow can trade. That is a
call-auction uncrossing price, and it is a definition of fair value grounded
in *flow*, not in theory:

- The fair price is not what a model says the contract is worth. It is the
  price at which trading activity balances on both sides.
- Corroborated independently by practitioner consensus (r/quant, 2026-07-06,
  saved as PDF in Jacob's Downloads): *"As a market maker you're mostly
  quoting at a price where trading activity is balanced on both sides instead
  of quoting at a theoretically 'correct' price"*; *"the 'IV' isn't a
  forecast — it's the clearing price"*; *"their edge is the spread plus
  leaning their quotes based on inventory, not selling at some computable
  break-even."*

The maker's job under this framing: find where flow balances, quote around
it, lean on inventory, collect the spread. Edge comes from the service, not
from out-predicting the market.

## 2. What we price off today, and why it is wrong

`FairValueInput` (source/pricing_model.hpp) is the entire universe the
pricing model can see:

```cpp
struct FairValueInput {
  double mid_cents;
  double time_to_close_hours;
  int net_position;
  std::optional<double> external_prob;
};
```

No tape. No taker sides. No depth beyond L1 (the `mid_cents` fed in is the
L1 micro-price, EMA-smoothed). Public flow influences *admission* (scanner
gates 61/62/63) and never *price*. The only flow the quoter reacts to is its
own fills (`FlowImbalanceGuard` records the bot's fills, not public trades)
plus a fixed 1c lean when that guard trips.

In demo books L1 is dominated by bot walls — resting size that never trades
and carries no information. The tape is the only participant set that put up
money. Measured cost of pricing off the walls:

- **Run 18**: fv said 80 while 100% of tape volume printed at 82 — a 2c bias.
- **The recurring drift line** — −$0.59 (run 19), −$0.42 (run 13), always
  marked against held inventory — is the same bias expressed in dollars.
- Every guard added since runs 13–19 (fade, lean, EMA depth) partially
  compensates for this anchor error rather than fixing it.

The quoter machinery itself is the right shape (spread capture + LMSR
inventory skew + item-64 unwind asymmetry = exactly "spread plus leaning on
inventory"). The broken component is the anchor those mechanisms hang off.

## 3. Translating "maximize crossed flow" to a resting binary book

A resting book is uncrossed by definition: executable volume is zero at every
price, so the interview answer cannot be applied literally. It decomposes
into two measurable analogues, one from the book and one from the tape.

### 3a. Book component — full-depth balance price

Build the YES-space ladder from both sides (YES bids are demand; NO bids at
`p_no` are implied YES asks at `100 − p_no`, per LocalOrderbook's existing
convention). Define cumulative curves:

- `D(p)` = total resting demand at prices ≥ p
- `S(p)` = total resting supply at prices ≤ p

The clearing price `p*` is where the curves balance — the price minimizing
`|D(p) − S(p)|`, tie-broken toward the mid. This is the micro-price
generalized from L1 to the full ladder: if one side is much deeper, `p*`
leans away from it (that side must concede to trade). `LocalOrderbook`
already holds the full ladder (`state_`); this is a cheap walk, no new data.

Open design question, settled by backtest not argument: deep walls far from
the touch are the *least* trustworthy size on these books. Candidate
weightings — flat full-depth, exponential decay by distance from mid
(`weight = λ^distance`), top-K levels — all go into the backtest grid.

### 3b. Tape component — where flow actually crossed

Every print is realized back-and-forth flow: a taker and a maker who agreed.
The tape component is a **time-decayed VWAP of recent public prints**
(half-life configurable), with two corrections:

- **Exclude our own fills.** Our prints sit at our quotes by construction —
  feeding them back in makes fv confirm itself. Match public `trade`
  messages against our `fill` messages by `trade_id` and drop them.
- **Sparse-tape fallback.** Below `min_tape_prints` in the window, the blend
  weight shifts to the book component (demo tapes can be thin; a two-print
  VWAP is noise).

### 3c. The blend

```
fv_raw = w_tape · tape_vwap + (1 − w_tape) · p*_book
```

EMA-smoothed exactly as today, then into the unchanged quoter pipeline
(reservation, skew, unwind, clamps). `w_tape`, tape half-life, depth
weighting, and min-print floor are config; the backtest picks the defaults.

## 4. What this deliberately does NOT change

- **Inventory machinery stays**: LMSR skew, inventory brake, position caps,
  item-64 unwind asymmetry. These price *risk*, not value, and compose with
  a better anchor (the unwind quote pegs to the reservation, which pegs to
  fv — better fv puts the unwind quote where run 18's takers actually were).
- **Admission gates stay**: flow-rate, live-spread, pinned-tape, expired-date
  (61/62/63). Selection and pricing are separate defenses.
- **ViewBasedModel stays off for quoting, permanently.** Pricing toward a
  privately "debiased" probability is quoting against the market's clearing
  price; the market's persistent bias is the risk premium a maker is paid to
  warehouse, not an error to trade against. If the Bürgi debias has value it
  is in market selection, not quotes.

## 5. Data inventory — the exchange already exposes everything this needs

Verified against [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md)
(live-doc pass 2026-07-02) and the live doc pages (2026-07-09):

| Source | Status in our code | Role in this plan |
|---|---|---|
| WS `orderbook_delta` | **In use** — primary feed into `LocalOrderbook` (full ladder) | Book component computes from what we already hold; zero new plumbing |
| WS `trade` channel | **Not subscribed** | Phase 0 — the tape, live; the one genuinely missing input |
| REST `GET /markets/{ticker}/orderbook` | In use (scanner finalist probe, item 61) | Unchanged; WS remains the in-session book source |
| REST batched multi-market orderbooks | **Unused** | Scanner probes N finalists in one call (folds into item 58); Phase 3 recording of non-quoted candidate markets without WS subscriptions |
| REST `GET /portfolio/orders/queue_positions` | **Unused** | `queue_position_fp` = contracts ahead at price-time priority, per resting order — see Phase 7 |
| REST market candlesticks (single + batch) | **Unused** | 1-min/1-h/1-day OHLC of trade price **and yes_bid/yes_ask**, volume, open interest — Phase 3's production-scale data tier |
| REST `GET /historical/trades` (+ `/historical/cutoff`) | **Unused** | Production trade tapes beyond the ~3-month live window — tape-VWAP calibration at scale |
| WS `market_ticker_v2` | **Not subscribed** | Incremental per-market summary deltas — watch a candidate watchlist live (rotation, item 65 flow features, item 60a drift) without full book subscriptions |
| WS `market_and_event_lifecycle` | **Not subscribed** | Push on market state changes — a determined/closed market exits the session instantly instead of waiting for the 5-min rotation poll (run-16 backstop at runtime) |

The queue-positions endpoint deserves emphasis: it extends "price with flow
in mind" from *where to quote* to *whether to move*. A resting order's queue
position is an asset denominated in flow — repricing abandons it. PLAN item
42b (queue-value reprice rule) was gated on a fill-probability model needing
~500 accumulated fills; exact queue positions plus the TradeTape flow rate
make queue value measurable **now**:

```
expected_time_to_fill ≈ queue_position_fp / tape_flow_rate(our_side, our_price)
```

## 6. Implementation plan (TDD throughout)

### Phase 0 — hear the flow (prerequisite, no behavior change)

The bot never receives public trades today: the WS subscribes only to
`orderbook_delta` and `fill` (source/websocket_client.cpp). Add the public
`trade` channel. Read `taker_outcome_side` / `taker_book_side` — the legacy
`taker_side` field is deprecated (API ref §order-direction; PLAN item 65's
mention of `taker_side` should follow suit). `CapturingWebSocket` tees every
inbound frame already, so captures pick up trades for free once subscribed.

### Phase 1 — `TradeTape` component

Rolling window of public prints per ticker: price, size, taker side,
timestamp; own-fill exclusion by `trade_id`. Queries, all `now`-parameterized
for testability like `FlowImbalanceGuard`:

- `vwap_cents(halflife)` — the tape component of fv
- `print_count(window)` — sparse-tape fallback input
- `minority_side_ratio(window)` — **feeds item 65 admission directly**; the
  scanner's REST trades probe can share the same accounting for startup,
  with the WS tape taking over in-session.

### Phase 2 — book clearing price

`LocalOrderbook::clearing_price_cents(DepthWeighting)` next to the existing
`micro_price_cents()`. Unit tests on synthetic ladders: balanced book →
mid; one-sided wall → leans away; empty side → falls back like micro; deep
far wall under decay weighting → bounded influence.

### Phase 3 — offline backtest (the go/no-go gate)

Record ≥2 live-slate capture sessions (full-depth deltas + trades — Phase 0
makes captures complete). Replay harness computes every candidate fv at each
book/tape event:

- micro (today's baseline) · full-depth `p*` (each weighting) · tape VWAP
  (each half-life) · blends (grid over `w_tape`)

Scored on: **(a) next-print prediction error** (MAE of fv vs the next public
trade price — did fv say where flow would cross?) and **(b) simulated drift**
(mark a unit of held inventory against each candidate fv; the run-13/18/19
loss line replayed). Deliverable: results table appended to this doc. The
winner on recordings — and only a winner — proceeds to Phase 4.

**Production-data tier (no recording needed, start immediately):** market
candlesticks carry OHLC of trade price *and* yes_bid/yes_ask per minute, and
`/historical/trades` serves real production tapes — so the tape-vs-book
question can be scored at candle granularity across hundreds of PRODUCTION
markets before our first demo capture finishes. Candle-scale results rank
the candidates; the tick-scale replay on our own captures confirms the
winner under quoting latency. The same candle set calibrates item 60a's
drift estimator on real markets instead of demo flow.

### Phase 4 — `ClearingPriceModel`

Third `IPricingModel`, config-selected (`pricing.model = "clearing"`),
HeuristicModel remains the default. Requires widening `FairValueInput` (or a
richer sibling struct) to carry the ladder and a tape summary — an interface
change, so tests first per TDD. Quoter is untouched apart from constructing
the input.

### Phase 5 — live A/B

Matched-market protocol as in runs 13/14 (same market family, consecutive
sessions, one variable). Success criteria, pre-declared:

- attribution **drift line ≤ half** of the matched HeuristicModel baseline
- **entry edge preserved** (within noise of +0.5–2.5c/lot)
- **round-trip completion rate up** (with item-64 unwind in place, the
  unwind quote finally rests where flow is)

### Phase 6 — knob consolidation (the "doing too much" dividend)

Only after Phase 5 passes, one removal per run, measured:

- **Fixed 1c flow lean** — subsumed by the tape component (the VWAP *is* the
  taker-side pressure signal, continuous instead of a tripwire constant).
- **Adverse theo-jump fade** — was reacting to L1 wall noise; with a
  tape-anchored fv, re-test whether it still fires on anything real.
- **EMA alpha** — a depth-and-tape fv is inherently steadier than L1 micro;
  the smoothing that damped wall noise may now just add lag.

docs/GUARDS.md consolidation plan tracks the outcome of each.

### Phase 7 — queue-value reprice rule (item 42b, newly unlocked)

With `queue_positions` (contracts ahead) and TradeTape (flow rate at our
price), the reprice decision becomes arithmetic instead of a timer:

- reprice only if `|fv − quoted| · fill_prob_at_new_price` exceeds the
  expected value of the queue position abandoned
- poll queue positions on the reprice decision path only (it costs REST
  tokens; never in the hot loop), or on rotation cadence
- the same number prices item 24's layered quotes (a layer's value IS its
  queue position) and tells the item-64 unwind quote how close its exit is

This phase needs Phases 0–1 (tape flow rate) but not 2–5; it can proceed in
parallel with the fv backtest if sequencing demands it.

## 7. Risks and honest caveats

- **Tape lag on real news.** After a genuine jump, recent prints anchor fv to
  stale levels. Mitigations: time-decay half-life, blend weight on the book
  component (which reprices instantly), and the fade guard stays until
  Phase 6 proves it redundant.
- **Thin demo tapes.** Some admitted markets may print a few times a minute.
  The sparse-tape fallback keeps fv defined; item 65 keeps us out of books
  where the tape is too one-sided for the strategy to work at all.
- **Self-referential tape.** If our maker fills dominate a book's prints,
  own-fill exclusion can leave near-nothing — correct behavior (fall back to
  book), but it flags the market as one where we ARE the flow, which item 65
  should treat as inadmissible.
- **Determined markets** print without price discovery (run 16); the item-62
  pinned-tape gate remains the defense — pricing assumes admission did its
  job. The `market_and_event_lifecycle` channel adds a runtime backstop:
  determination pushes an exit instead of waiting for rotation.
- **Integer-cent assumption.** Kalshi deprecated legacy integer-cent price
  fields (2026-03-05) and offers per-market **subpenny pricing** near the
  0/100 tails — exactly where the longshot guard operates. The codebase
  prices in `int` cents throughout. `ClearingPriceModel` should compute in
  fixed-point dollars from day one, and the quoter's tick-grid assumption
  needs an audit item in PLAN before any live switch on a subpenny market.

## 8. Sequencing against open work

PR #94 (unwind pricing) is independent — merge order irrelevant. Item 65
(two-sided admission) shares Phase 0+1 plumbing and should ride the same
branch sequence. The L1 VM (PLAN item 3) is orthogonal but capture sessions
for Phase 3 are better run on it — laptop sleep truncates recordings.
