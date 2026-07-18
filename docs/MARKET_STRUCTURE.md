# Kalshi Market Structure & Strategy Taxonomy

Distilled from a claude.ai design discussion (July 2026), curated against this
repo's first-party knowledge. Where the original said VERIFY and we have a
verified answer, the answer is inlined with its source; the still-open items
are collected in §6. Complements:
[KALSHI_API_REFERENCE.md](KALSHI_API_REFERENCE.md) (API ground truth) ·
[GUARDS.md](GUARDS.md) (defensive mechanisms) ·
[BETTER_PRICING.md](BETTER_PRICING.md) (fair-value research) ·
[papers/](papers/README.md) (external research).

## 1. Contract and collateral model

- Binary event contracts settle at $1 (yes) / $0 (no); price in cents =
  implied probability. Buying NO at (1−p) is operationally identical to
  selling YES at p — one instrument, two framings (this is why the quoter's
  ask is a NO buy).
- **Fully collateralized, no margin.** Worst-case loss is posted at order
  placement: buy YES @ 40c locks 40c; the NO side of a matched pair locks
  60c. No variation margin, no liquidation engine. Portfolio worst case =
  posted collateral, exact to the cent.
- **Collateral locks on RESTING orders, not just fills.** Quoting breadth is
  bounded by gross open-order collateral; unfilled quotes have a real
  carrying cost. Layered quoting (PLAN item 24) and any multi-market scaling
  must budget collateral per resting lot, not per position.
- Netting: YES + NO in the same market nets flat. Whether all-NO baskets
  across a mutually exclusive event group net (≈ pennies) or stack (≈ $n) is
  OPEN (§6) — order-of-magnitude ROC impact on partition trades.
- Capital in long-dated markets is locked until exit or resolution:
  time-to-resolution is a first-class cost (the scanner's
  `max_days_to_close` gate is the current, crude expression of this).

## 2. Market-making economics

- The 1c tick is enormous relative to notional (2% of mid at 50c). Spreads
  are structurally fat; competition is queue position and cancel speed under
  price-time priority, not sub-tick pricing. (Queue priming = PLAN item 24;
  amend-first repricing already preserves round-trips.)
- Gross capture ≈ 2% per round-trip cycle on a 2c spread — excellent in
  daily-cycle markets (sports, crypto/index dailies, econ prints),
  annualizes terribly in six-month markets. **Concentrate in short-cycle
  markets** (supports PLAN item 71, the crypto 15-minute series).
- Effective return = capture × fill rate × capital velocity. The four drags:
  1. **Utilization** — most resting quotes never fill.
  2. **Duration** — long-dated illiquid markets are the worst capital use.
  3. **Lumpy adverse selection** — one toxic fill at a data release costs
     30–50c ≈ 15–25 round trips of capture. Toxic-fill avoidance around
     scheduled events dominates spread width for net PnL (aligns with runs
     13/21/22 attribution: drift/pick-off losses dwarf per-fill edge; guards:
     theo-jump fade, panic pull = item 51, max-hold = item 70).
  4. **Opportunity cost** — idle collateral forgoes T-bill yield; that is
     the hurdle rate. Whether Kalshi pays interest on balances: OPEN (§6).
- Fees: maker fills free on most markets (verified live — API reference §8.5:
  `fee_cost` present per fill, 0 for makers; per-series maker-fee exceptions
  exist, which is what `QuoterConfig.maker_fee_rate` and PLAN item 23 cover).
  Taker ≈ 0.07·p·(1−p) per contract (max ~1.75c near 50c) — a moat around
  resting quotes. Exact current schedule per series: check the fee-changes
  endpoint (API reference §5).
- Capacity: no leverage → PnL scales at best linearly in capital; the
  extractable pool is bounded by exchange volume × capture. Head markets
  have volume plus real competition (Susquehanna has been a designated MM);
  tail markets have 3–10c spreads, no competition, tiny absolute dollars.
  Risk of ruin is capped at deployed collateral → size cleanly, run near
  full deployment.

## 3. Pricing regimes

- Fair value = risk-neutral event probability, minus a small carry discount
  for long-dated contracts.
- Most markets (elections, Fed, weather) have **no hedgeable underlying**:
  no replication, edge = forecast quality + flow management. Closer to
  bookmaking than options MM (consistent with Palumbo's LP-as-underwriting
  result, papers §1).
- Index/crypto range markets are digital calls: FV ≈ N(d2). **Digital gamma
  explodes ATM near expiry** — price whips 50→90 on small spot moves. This
  is the sharpest risk on the platform; widen or pull into the close (PLAN
  item 71's "quote early/mid window, flee the end" is this fact).
- Information arrives in scheduled lumps (CPI 8:30 ET, FOMC 14:00 ET, game
  end) plus jump news. Fills in the seconds around a known timestamp are
  toxic by default: widen/pull around scheduled releases (a cheap,
  currently-unbuilt guard — calendar-aware quoting).
- Pinning: mature markets trade 95–99c — pennies of capture against tail
  risk (the scanner's price band + longshot guard already avoid these).
  Deadline drift: price migrates toward 0/100 from pure non-events before a
  cutoff.

## 4. Cross-market structure: the constraint graph

**The most actionable new strategy family in this doc** — and post-item-76 it
fits the architecture as a second `IStrategy` (or a sibling strategy library)
over the same engine/exchange stack. Core stance: the constraint graph IS the
pricing model, not a separate arb bot. Constraint-implied fairs price
illiquid legs off liquid ones; violations get captured passively by standing
quotes; aggressive completion only when a violation exceeds fees + threshold.

- **Partitions (Dutch books).** Mutually exclusive + exhaustive YES prices
  must sum to $1. Asks sum < $1 → buy all YES; bids sum > $1 → buy all NO.
  The no-arb band is as wide as summed taker fees (~3.5c+ for
  near-the-money strips), so on-screen "arbs" are mostly inside the band or
  bait — passive strip completion is the strong version of the trade.
- **Ladders / monotonicity.** Threshold markets form a discrete risk-neutral
  CDF: "CPI ≥ 3.0" contains "CPI ≥ 3.1", so containing ≥ contained, always.
  Fast markets produce inversions (books update asynchronously): buy the
  container at ask q, sell the contained at bid r > q → lock r−q plus a free
  $1 lottery if the outcome lands in the gap. Range buckets are threshold
  differences; deadline ladders ("by March" ⊆ "by June") are identical.
- **Logical containment (hand-curated).** P(wins presidency) ≤ P(wins
  nomination): hard payoff dominance. Keep strictly separate from
  correlation trades (recession vs GDP) — those are stat-arb with real risk,
  not dominance.
- **Cross-venue.** CME fed funds futures give independent implied Fed
  probabilities (signal + approximate hedge; basis: average-daily-rate
  settlement vs binary). Polymarket resolves via UMA's optimistic oracle vs
  Kalshi's CFTC rulebook → rules basis (US access post-QCEX: OPEN, §6).
  Sportsbooks: devig by normalizing two sides; they ban winners. Robinhood's
  prediction hub routes to Kalshi's own book — same venue, no arb. IBKR
  ForecastEx is a genuinely separate CFTC exchange.
- **Rules risk is an edge category.** Settlement fine print decides trades:
  initial BLS print vs revisions, AP call vs certification, one specific NWS
  station. "Same event" across venues can be two different contracts in
  exactly the states that matter.
- **Execution reality**: no atomic multi-leg order type → legging risk. Size
  to minimum depth across legs, fire IOC in a batch (PLAN item 54, batch
  CreateOrders, is the enabler), carry an unwind plan.
- Prerequisite for auto-generating the graph: event metadata
  (mutual-exclusivity flags, strike structure). The API's multivariate-event
  endpoints and `price_level_structure` (PLAN item 69) are the starting
  point — OPEN item in §6.

## 5. Infrastructure notes

- Matching engine runs in AWS. **Region: us-east-2 (Ohio) — first-party
  answer from Kalshi Institutional (Brad, 2026-07-11)**, superseding older
  "us-east-1" folklore. No colo product; "colocation" = same region.
- Pick the AZ empirically: fire test orders, take the argmin of the
  ack-latency distribution. AZ letter names are randomized per AWS account —
  compare AZ IDs, not letters (belongs in the L1 checklist,
  [AWS_SETUP.md](AWS_SETUP.md)).
- The latency regime is millisecond-scale with real jitter (API gateway in
  path): the crypto-exchange playbook, not CME. In-region placement, fastest
  order interface, warm connections, rate-limit budgeting, cancel discipline
  around events. Microsecond link optimization buys nothing. (Matches our
  measured L0: keep-alive cut order RTT ~255→~22ms locally.) FIX exists but
  gates on ≥5% exchange monthly volume (PLAN north-star); WS/REST tiers:
  Premier 1k/1k · Paragon 2k/2k · Prime 4k/4k per second, by volume share.
- Most cross-market arb is **intra-exchange**: every partition/ladder/
  containment leg lives in one matching engine — one box holds all books in
  memory, no geography problem. This makes §4 buildable on the current
  single-process architecture.
- Genuine cross-venue architecture (if/when Gate 2 opens): one execution
  node in Kalshi's region owning all Kalshi books + the constraint graph;
  thin remote feed handlers (CME vendor feed, Polymarket WS) forwarding
  compressed signals point-to-point into it. No node ever sees a globally
  consistent multi-venue view — place autonomy where reaction must happen,
  eat staleness on remote replicas. Slow path (global risk, recon, PnL,
  parameter distribution, kill switches) reconciles asynchronously off the
  hot path — the same split as [ADR-007](adr/007-process-per-strategy-and-aggregator.md).

## 6. Still open — verify before coding against

- Collateral netting rules within mutually exclusive event groups (decides
  partition-trade ROC by an order of magnitude).
- Interest on idle account balances (sets the hurdle rate).
- Current per-series fee schedule and maker-fee exceptions (fee-changes
  endpoint).
- Measured ack-latency distribution by AZ ID in us-east-2 (fold into L1).
- Polymarket US access status post-QCEX acquisition.
- API event metadata sufficient to auto-generate the constraint graph:
  mutual-exclusivity flags, strike floors/caps, MVE collection semantics.
