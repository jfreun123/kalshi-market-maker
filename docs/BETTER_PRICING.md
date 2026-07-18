# Better Pricing — quoting where the flow is

> Design doc for PLAN items 66 (clearing-price fair value) and 65
> (two-sided-flow admission), plus the cleanup both unlock. Written to be
> readable without a market-making background — terms are explained as they
> appear. Companion to [PLAN.md](PLAN.md) · guards catalog:
> [docs/GUARDS.md](docs/GUARDS.md) · API ground truth:
> [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md).

## 0. Vocabulary

Ten terms everything else builds on:

- **The book** — the list of everyone's outstanding buy offers (**bids**) and
  sell offers (**asks**) on a market, sorted by price. One price plus the
  total size resting at it is a **level**; all the levels together are the
  **ladder**; the size sitting at levels behind the best price is **depth**.
  **L1** ("top of book") means looking at only the single best bid and best
  ask and ignoring everything behind them.
- **Spread / mid** — the gap between best bid and best ask, and the halfway
  point between them.
- **Maker / taker** — a maker posts a resting order and waits to be traded
  with; a taker crosses the spread to trade instantly against a resting
  order. Our bot is a maker: it earns by being paid the spread for waiting.
- **The tape / a print** — the public record of completed trades. Each trade
  "prints" at a price; on Kalshi each print also records which side the
  taker was on.
- **Fair value (fv)** — the bot's single-number belief about what a contract
  is worth right now. Every quote is placed relative to it: bid a little
  below fv, ask a little above.
- **Inventory** — the position we're currently holding. Flat = zero. Holding
  inventory is risk: if the price moves against it, we lose.
- **Edge** — how much better than fair value a fill is at the moment it
  happens. Buying at 78 when fv is 80 is +2 cents of edge.
- **VWAP** — volume-weighted average price of trades: an average where
  bigger trades count more.
- **EMA** — exponential moving average: a way to smooth a jumpy number by
  weighting recent observations more than old ones.
- **Getting picked off (adverse selection)** — being filled by someone who
  knew the price was about to move: your fill looks fine for one second and
  is a loser ten seconds later.

## 1. The idea

An HFT interview question (Jacob's): *market participants share limit orders
that may cross. Given all the orders at once, find the single fair price.*
The answer: **the price that lets the most volume trade** — where the total
amount buyers want to buy meets the total amount sellers want to sell. In
exchange language this is the **uncrossing** or **clearing price** (it is
literally how opening auctions on stock exchanges set the opening price
each morning).

The point of the question is a definition of fair value grounded in *flow*
(actual willingness to trade) rather than in theory:

> The fair price is not what a model says the contract is worth. It is the
> price at which trading activity balances.

Working market makers say the same thing (r/quant thread, 2026-07-06, PDF
saved by Jacob): *"you're mostly quoting at a price where trading activity
is balanced on both sides instead of quoting at a theoretically 'correct'
price"*, and *"their edge is the spread plus leaning their quotes based on
inventory, not selling at some computable break-even."* Translation: a
maker's profit is the fee it collects for providing the service (the
spread), plus adjusting its prices based on what it happens to be holding.
Its profit is not out-predicting the market — and quoting a "smarter"
private opinion of the price just means informed traders trade through you
until you agree with everyone else.

## 2. How the bot prices today, and why that's the bug

Today fv comes from the **micro-price**: take the best bid and best ask,
and weight each price by the size resting on the *opposite* side, so the
answer leans toward whichever side has more pressure. Then smooth it with
an EMA. Reasonable — but it only looks at L1, the top of the book.

This is verifiable at the code level. The struct below is *everything* the
pricing model is allowed to see (`source/pricing_model.hpp`):

```cpp
struct FairValueInput {
  double mid_cents;
  double time_to_close_hours;
  int net_position;
  std::optional<double> external_prob;
};
```

No tape. No public trades. No depth beyond the one blended number. The
public trade stream reaches the *scanner* (to decide which markets to
enter) but never the *price*. The only flow the quoter reacts to is its own
fills.

Why that's fatal on these books: the resting size at the top of demo books
is mostly **bot walls** — big orders placed by other bots that never
actually trade. Size without information. The tape is the only set of
participants who actually put money on the line. Measured cost of pricing
off the walls instead of the flow:

- **Run 18**: our fv said 80 while 100% of the hour's volume printed at 82.
  A 2-cent blind spot.
- **The recurring "drift" loss** — drift is the loss from marking our held
  inventory as the price moves against it — was −$0.59 in run 19 and
  −$0.42 in run 13, always against the inventory we were holding. That's
  the same 2-cent blind spot expressed in dollars: we hold positions priced
  by the walls while the flow walks away from us.
- Several guards added over runs 13–19 (the fade, the fixed lean, heavy
  smoothing) partially *compensate* for the biased anchor rather than fix it.

Important: the machinery *around* fv is the right shape — collect the
spread, shift quotes with inventory, exit passively (item 64). It matches
exactly what the practitioners describe. The broken part is the one number
everything hangs off.

## 3. Turning "maximize matched flow" into something computable

One subtlety first: an open market's book never overlaps — if a bid ever
met an ask, the exchange would have matched them already. So on a resting
book, "how much volume could trade right now" is zero at every price, and
the interview answer can't be applied literally. It decomposes into two
measurable stand-ins: one from the book, one from the tape.

### 3a. Book side — where cumulative supply meets demand

Kalshi books are two lists of bids: YES bids and NO bids. A NO bid at 44¢
is exactly an offer to sell YES at 56¢ (the two sides of the same coin), so
we can redraw the whole book on one YES scale: buyers below, sellers above.

Now walk the ladder and accumulate. Tiny example:

```
sellers (asks):  30 @ 68        cumulative selling at ≤68: 50
                 20 @ 67        cumulative selling at ≤67: 20
                 ---- spread ----
buyers (bids):  100 @ 62        cumulative buying at ≥62: 100
                 40 @ 60        cumulative buying at ≥60: 140
```

The mid is 64.5. But there is 140 of buying interest against 50 of selling
interest — if flow arrived, the sellers would run out first, so the "price
where the two sides balance" sits near 67, not 64.5. That balance point —
the price where cumulative demand and cumulative supply are closest to
equal — is the **book clearing price**. The micro-price is this exact idea
computed from only the first row of each side; we're generalizing it to the
whole ladder. `LocalOrderbook` already stores the whole ladder, so this
costs nothing new.

One honest wrinkle: deep walls far from the touch are the *least*
trustworthy size on these books. So we'll test a few weightings — count all
depth equally, discount levels by distance from the mid, or use only the
top K levels — and let the backtest (Phase 3) pick, not an argument.

### 3b. Tape side — where flow actually crossed

Every print is a real matched trade: a taker and a maker who agreed on a
price with money attached. The tape component of fv is a **time-decayed
VWAP of recent public prints** — an average trade price where bigger trades
count more and recent trades count more (half-life configurable: a print
from N seconds ago counts half as much as one from now). Two corrections:

- **Down-weight our own fills** (`own_fill_weight`, default 0 = excluded).
  Our trades print at our own quoted prices by construction, so feeding
  them back in at full weight makes fv agree with wherever we already were
  — the worst case is a picked-off stale quote generating a print that
  confirms the stale price. But every print of ours has two halves: our
  price (our own opinion recycled — poker: you don't read the villain's
  range off *your* bet) and the taker's decision to cross at it (real flow
  — you *do* update when the villain calls). Jacob's challenge, 2026-07-09.
  The taker half already reaches fv through the inventory skew and flow
  lean, so 0 is the safe default; whether partial credit in the VWAP beats
  routing it only through skew/lean is a backtest question — `own_fill_
  weight` joins the Phase 3 grid alongside `w_tape` and the half-life.
  We match public prints against our own fill stream by `trade_id`.
- **Thin-tape fallback.** If fewer than `min_tape_prints` occurred in the
  window, a two-print average is noise — the blend weight shifts toward the
  book component.

### 3c. The blend, and a real example

```
fv_raw = w_tape · tape_vwap + (1 − w_tape) · book_clearing_price
```

then EMA-smoothed exactly as today, then into the unchanged quoter. The
weight, half-life, depth weighting, and print floor are config; the
backtest picks defaults.

Here is the bias this fixes, in one real production candle (a 1-hour
summary bar fetched live on 2026-07-09; see §5 for what candles are), from
the "Dodgers beat Diamondbacks Jul 12" market:

```
quoted best bid over the hour:  61 → 63
quoted best ask over the hour:  67 → 66 → 67
trades this hour:               ~50 contracts, ALL at 66–67 (mean 66.98)
```

The quoted mid says fv ≈ 65. The flow says the market clears at ≈ 67 —
buyers lifted the offer all hour while the bid crawled up behind them. A
maker anchored to the mid would think its 67 ask carries 2 cents of edge
(it carries none — 67 IS the price) and would rest its bid near 63, four
cents below where anyone is trading, never filling. That is run 18's
pattern, visible in public production data.

## 4. What this deliberately does NOT change

- **Inventory machinery stays.** When we accumulate a position, the quoter
  shifts both quotes to encourage the market to take it off our hands (the
  "reservation price" — fv adjusted for what we hold), stops adding past a
  cap, and prices the exit at fair value instead of demanding extra profit
  to close (item 64). All of that is pricing *risk*, not value, and it
  composes with a better anchor: the exit quote pegs to the reservation,
  the reservation pegs to fv — fix fv and the exit quote finally rests
  where the takers actually are.
- **Admission gates stay** (items 61/62/63: enough recent trades, wide
  enough spread, a tape that actually moves). Choosing markets and pricing
  them are separate defenses.
- **ViewBasedModel stays off for quoting, permanently.** That model prices
  toward what research says the *true* probability is (correcting the
  known favorite-longshot bias in prediction markets). The practitioner
  point from §1 applies: the market's persistent bias is the risk premium a
  maker is *paid to warehouse*, not an error to quote against. If the
  debias has value, it's in choosing markets, not in setting quotes.

## 5. Data inventory — the exchange already exposes everything this needs

Verified against [docs/KALSHI_API_REFERENCE.md](docs/KALSHI_API_REFERENCE.md)
(live-doc pass 2026-07-02) and the live doc pages (2026-07-09). Two terms
used below: **candlesticks** are per-interval summary bars (for each 1-min
/ 1-hour / 1-day bucket: first, highest, lowest, and last price — for
trades AND for the quoted bid and ask — plus volume); **queue position** is
how many contracts sit ahead of our resting order at the same price — the
exchange fills orders first-come-first-served within a price level
("price-time priority"), so it measures how close our order is to trading.

| Source | Status in our code | Role in this plan |
|---|---|---|
| WS `orderbook_delta` | **In use** — feeds `LocalOrderbook` (full ladder) | Book clearing price computes from what we already hold; zero new plumbing |
| WS `trade` channel | **Not subscribed** | Phase 0 — the live tape; the one genuinely missing input |
| REST `GET /markets/{ticker}/orderbook` | In use (scanner probe) | Unchanged; WS remains the in-session book source |
| REST batched multi-market orderbooks | **Unused** | Scanner probes N finalists in one call (folds into item 58); records candidate markets we aren't quoting |
| REST `GET /portfolio/orders/queue_positions` | **Unused** | Exact queue position per resting order — see Phase 7 |
| REST market candlesticks (single + batch) | **Unused** | Per-minute trade AND bid/ask history for any market — Phase 3's production-scale data, no recording needed |
| REST `GET /historical/trades` (+ `/historical/cutoff`) | **Unused** | Production trade tapes older than the ~3-month live window |
| WS `market_ticker_v2` | **Not subscribed** | Lightweight per-market summaries — watch a candidate watchlist live without full book subscriptions (rotation, item 65, item 60a) |
| WS `market_and_event_lifecycle` | **Not subscribed** | Push when a market's state changes — a determined/closed market exits the session instantly instead of waiting for the 5-minute rotation poll (run-16 backstop) |

The queue-positions endpoint deserves emphasis: it extends "price with flow
in mind" from *where to quote* to *whether to move*. A resting order's spot
in line is an asset — repricing means cancelling and going to the back of a
new line. PLAN item 42b (only reprice when it's worth abandoning the queue)
was blocked waiting for a statistical model needing ~500 recorded fills;
exact queue positions plus the tape's flow rate make it simple arithmetic
**now**:

```
expected_time_to_fill ≈ contracts_ahead_of_us / rate_of_prints_at_our_price
```

## 6. Implementation plan (TDD throughout)

### Phase 0 — hear the flow (prerequisite, no behavior change)

Subscribe the WebSocket to the public `trade` channel (today the bot
listens only to book updates and its own fills — it literally never hears
other people's trades in real time). Read the current field names
`taker_outcome_side` / `taker_book_side` (the older `taker_side` is
deprecated). The capture wrapper already records every inbound message, so
recordings pick up trades for free once subscribed.

### Phase 1 — `TradeTape` component

A rolling per-market window of public prints: price, size, taker side,
timestamp; our own fills excluded. Queries: `vwap_cents(halflife)`,
`print_count(window)`, `minority_side_ratio(window)` — that last one is
item 65's admission input (what fraction of recent prints came from the
less-active side; if ~0, every taker is on the same side and round trips
are impossible there).

### Phase 2 — book clearing price

`LocalOrderbook::clearing_price_cents(weighting)` next to the existing
`micro_price_cents()`. Unit tests on synthetic ladders: balanced book →
mid; one-sided depth → leans away from it; empty side → same fallback as
micro; a huge far-away wall under distance-decay → bounded influence.

### Phase 3 — offline backtest (the go/no-go gate)

Score every fv candidate on data before touching live behavior:

- **Candidates**: today's micro-price · book clearing price (each
  weighting) · tape VWAP (each half-life) · blends (grid over `w_tape`).
- **Scores**: (a) next-print error — after each book/tape event, how far
  was the candidate fv from the *next* trade's price? (did it say where
  flow would cross?) and (b) simulated drift — mark one held contract
  against each candidate fv over the session; the run-13/18/19 loss line
  replayed.

**Production-data tier (start immediately, nothing to record):**
candlesticks give per-minute trade prices AND quoted bid/ask for any
production market, and `/historical/trades` serves real tapes — so the
tape-vs-book question can be scored across hundreds of production markets
at minute resolution before our first demo recording finishes. Candle-scale
results rank the candidates; a tick-scale replay on our own recordings
confirms the winner under real quoting conditions. The same candle data
calibrates item 60a's trend detector on real markets instead of demo flow.

Deliverable: a results table appended to this doc. The winner on data — and
only a winner — proceeds to Phase 4.

**Phase 3 candle-tier results (run 2026-07-09, `scripts/backtest_fv_candles.py`,
30 highest-volume production markets, 8h, 4,014 scoring events):**

| candidate | MAE (¢) | bias (¢) |
|---|---|---|
| mid (book anchor) | **1.62** | −0.23 |
| blend(w_tape=0.25, h=5m) | 1.96 | **−0.03** |
| last trade | 2.10 | −0.03 |
| blend(w_tape=0.5, h=5m) | 2.60 | +0.16 |
| tape VWAP (h=2m) | 3.11 | +0.24 |
| tape VWAP (h=5m) | 4.17 | +0.54 |
| tape VWAP (h=15m) | 5.91 | +0.75 |

The ranking holds in **every** spread bucket (≤1.5¢ / 1.5–3.5¢ / >3.5¢) —
wide books did not flip it. Two conclusions:

1. **The book stays the anchor.** The quoted mid beats every tape variant
   on accuracy; the tape lags by construction (prints are history). The
   strong form of this plan — a tape-anchored fv — is refuted at minute
   scale on liquid production books.
2. **The tape's job is bias correction.** The mid's error is not centered:
   trades print systematically above it in tight/medium books (−0.22¢ /
   −0.40¢ — run 18's direction, in production). A small blend
   (w_tape ≈ 0.1–0.25) zeroes the bias at modest MAE cost. Bias, not MAE,
   is what converts to drift dollars against held inventory — MAE ~1.6¢ is
   mostly unavoidable bid-ask bounce (trades can only print at somebody's
   quote, ~a half-spread from any honest fv).

**Decision: Phase 4's `ClearingPriceModel` must not be tape-heavy.** Default
shape: clearing-price book anchor (robustness vs walls) + low-w tape
correction (bias). The dose (`w_tape`, `own_fill_weight`, depth weighting,
half-life) is confirmed at tick scale on our own demo captures with the
simulated-drift score — thin bot-walled demo books may want more tape than
these liquid books did; that remains the open question for Phase 3b.

**Phase 3b harness (shipped 2026-07-10):** `kalshi_mm --fv-replay
<capture/session.jsonl>` replays a `--capture` recording through the
production parse path, `LocalOrderbook`, and `TradeTape` (via
`WebSocketClient::inject_frame` over a `NullWebSocket` — no credentials or
config), and prints per-candidate MAE/bias. Scoring is **markout-style**:
each print queues an event graded against the first same-market print ≥
`score_horizon_seconds` (default 30) later — grading against the
immediately-next print rewards chasing the tape, because demo prints
arrive in bursts at one price (measured: the pure-10s-tape candidate
scores 0.31c at horizon 0, an autocorrelation artifact, vs 0.66c at 30s).

**Phase 3b first capture (2026-07-10, 30 min, 3 self-selected demo
markets — PGA in-play + 2 pre-game WC mention markets, 1,070 scored
prints, 30s horizon):**

| candidate | MAE (¢) | bias (¢) |
|---|---|---|
| tape(w=1, h=10s), any anchor | **0.66** | −0.61 |
| micro+tape(w=0.75, h=10s) | 0.93 | −0.89 |
| micro+tape(w=0.5, h=30s) | 1.49 | −1.45 |
| micro (pure) | 1.73 | −1.71 |
| clearing(d=0.5) (pure) | 1.95 | −1.73 |
| clearing(flat) (pure) | 2.72 | −1.35 |

Findings, and their limits:

1. **On demo books the tape dose inverts vs production**: more tape =
   better, fresher = better, and pure L1 micro beats both clearing
   variants (demo far-depth is junk walls — flat clearing is the worst
   anchor on the board; distance decay only partially repairs it).
2. **Every candidate's bias is negative** (−0.6 to −1.7¢): prints landed
   above every fv formula all window long — persistent one-way buying in
   the *public* flow, run 19's one-sidedness measured in market data.
3. **The structural caveat that caps what this metric can decide**: in
   one-way flow, every print is on the taker's side, so print-prediction
   at ANY horizon rewards estimators that sit at the level being hit —
   which is also exactly where a maker gets run over. The offline metric
   has reached its discriminating limit; the tape-heavy winner here and
   the book-heavy winner on production candles bracket the answer, and
   **dollars must decide: Phase 5's live A/B on matched markets is the
   real test**, with round-trip completion and drift as the score.

### Phase 4 — `ClearingPriceModel` (shipped 2026-07-10)

`fv = (1 − w) · anchor + w · tape_vwap`, where the anchor is the quoter's
existing EMA-smoothed micro-price and the tape term is the raw decayed VWAP
of recent public prints (no prints → pure anchor). Selected by
`quoter.use_clearing_pricing`; knobs `clearing_tape_weight` (default 0.5)
and `tape_half_life_seconds` (default 30) — the midpoint of the offline
bracket, since liquid production books favored ~0.25 and thin one-way demo
books rewarded up to 1.0. The quoter queries the live `TradeTape` and
passes the VWAP through `FairValueInput::tape_vwap_cents`; skew, lean, and
all guards apply downstream unchanged. **Off by default — Phase 5's A/B
flips it only if the dollars agree.** Two deviations from the original
sketch, both data-driven: the anchor stays micro (not the full-depth
clearing price — demo far-depth proved to be junk walls), and prices remain
integer cents for now (the subpenny audit is PLAN item 69).

### Phase 5 — live A/B

Same protocol as runs 13/14: same market family, consecutive sessions, one
variable changed. Success criteria, declared before the run:

- the drift loss line at most **half** of the matched baseline session
- entry edge preserved (within noise of +0.5–2.5c per fill)
- more completed round trips (buy AND sell the same inventory as a maker —
  with item 64's exit pricing in place, the exit quote finally rests where
  the flow is)

### Phase 6 — knob consolidation (the "doing too much" dividend)

Only after Phase 5 passes, one removal per run, measured:

- **the fixed 1-cent flow lean** — the tape VWAP is the same signal,
  continuous instead of a tripwire;
- **the adverse-jump fade** — it reacted to L1 wall noise; retest whether
  it still fires on anything real once fv stops listening to walls;
- **EMA depth** — a depth-and-tape fv is steadier than L1 micro by
  construction; smoothing that damped wall noise may now just add lag.

docs/GUARDS.md's consolidation plan tracks each outcome.

### Phase 7 — queue-value reprice rule (item 42b, newly unlocked)

With exact queue positions (§5) and the tape's flow rate, "should we move
our quote?" becomes arithmetic: reprice only if the value of quoting at the
better price exceeds the value of the queue spot we'd abandon. Poll the
endpoint only when deciding a reprice (it costs rate-limit tokens), never
in the hot loop. The same number prices item 24's layered quotes and tells
item 64's exit quote how close it is to filling. Needs Phases 0–1 only;
can run in parallel with the fv backtest.

## 7. Risks and honest caveats

- **The tape lags real news.** After a genuine jump, recent prints anchor
  fv to stale levels for a few seconds. Mitigations: the time decay, the
  book component (which reprices instantly), and the fade guard stays until
  Phase 6 proves it redundant.
- **Thin demo tapes.** Some admitted markets print a few times a minute.
  The fallback keeps fv defined; item 65 keeps us out of books where the
  tape is so one-sided the strategy can't work at all.
- **We might BE the tape.** If our own fills dominate a book's prints,
  excluding them leaves near-nothing — correct behavior (fall back to the
  book), but it flags a market where we are the only liquidity, which item
  65 should treat as inadmissible.
- **Determined markets** (the outcome is already decided in the real world)
  print without price discovery — run 16's trap. The item-62 gate remains
  the defense at admission; the `market_and_event_lifecycle` channel adds a
  runtime backstop by pushing the state change the moment the exchange
  knows.
- **Integer-cent assumption.** Kalshi deprecated integer-cent price fields
  (2026-03-05) and offers per-market **subpenny pricing** (prices finer
  than 1 cent) near the 0/100 tails — exactly where our longshot guard
  operates. The codebase prices in `int` cents throughout.
  `ClearingPriceModel` computes in fixed-point dollars from day one, and
  the quoter's 1-cent grid assumption needs an audit item in PLAN before
  any live switch on a subpenny market.

## 8. Sequencing against open work

PR #94 (unwind pricing) is independent — merge order irrelevant. Item 65
(two-sided admission) shares Phase 0+1 plumbing and should ride the same
branch sequence. The L1 VM (PLAN item 3) is orthogonal, but Phase 3's
capture sessions are better run on it — laptop sleep truncates recordings.
