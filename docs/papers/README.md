# Research Papers & Notes

Reference papers on prediction-market microstructure, market-making, and pricing,
with study notes and their relevance to this project. The PDFs live alongside
this file (gitignored — binaries/copyrighted; keep locally). Cite these from
`PLAN.md` rather than duplicating the detail.

Notes re-verified against the PDFs on 2026-07-03 (page-ref'd; several earlier
claims corrected — see the ⚠ marks). Bot-improvement items extracted from this
pass live in the archived full plan
(`docs/archive/PLAN-2026-07-04-full.md`); surviving items carry their numbers
into the lean `PLAN.md`.

| File | Paper | Type |
|---|---|---|
| `burgi-deng-whelan-2026-makers-and-takers-kalshi.pdf` | Bürgi, Deng & Whelan (2026), *Makers and Takers: The Economics of the Kalshi Prediction Market* | Academic (UCD / GWU WP 2026-001) |
| `bawa-2025-prediction-market-alpha.pdf` | Bawa (2025), *The Mathematical Execution Behind Prediction Market Alpha* | Practitioner (Substack) |
| `berg-proebsting-2009-hanson-automated-market-maker.pdf` | Berg & Proebsting (2009), *Hanson's Automated Market Maker* | Academic (J. Prediction Markets 3(1):45–59) |
| `designing-prediction-markets-lesswrong-2026.pdf` | ToasterLightning (2026), *Designing Prediction Markets* | Blog (LessWrong) |
| `marketmaking.pdf` | Chakraborty & Kearns (2011), *Market Making and Mean Reversion* | Academic (ACM EC'11) |

---

## 1. Bürgi, Deng & Whelan (2026) — *Makers and Takers: The Economics of the Kalshi Prediction Market*

The most directly relevant paper: the first systematic empirical study of Kalshi
pricing, on transaction-level data for 46,282 contracts / 12,403 events /
313,972 prices (2021–Apr 2025), plus a microstructure model of the maker/taker
split. Sample deliberately ends April 2025, when Kalshi introduced maker fees
(p.6).

**Key empirical findings**
- **Prices are informative** and get more accurate approaching close — but show a clear **favorite–longshot bias (FLB)** at *every* horizon 0–10 days out (Table 5, p.22: ψ 0.017–0.041, all significant), and the bias **survives every volume and trade-size quintile** (Tables 6–7, p.23 — the largest-transaction quintile has the *largest* ψ, 0.043). High volume does not buy price efficiency.
- **Longshots lose badly:** contracts priced **< 10¢ lose > 60%** of stake (p.2, §3.3 p.17). Average return across all contracts ≈ **−20% pre-fee** (p.17). Positive returns exist above 50¢, but statistical significance is only established **above 70¢** (§3.3 p.17). ⚠ *earlier note said ">50¢ stat. sig." — that's the intro's loose phrasing, not the result.*
- **Makers beat Takers:** Maker avg −9.64% vs Taker −31.46% (p.27). Note makers still *lose* on average — they only profit on high-priced contracts. Kalshi's data flags maker vs. taker directly (no Lee–Ready guessing).
- **Maker edge is repricing:** makers out-return takers because they trade at better prices *and* "must also be willing to cancel those orders if new evidence emerges" (p.27). The taker pool is the extreme-belief flow that hits stale quotes.
- **Winning maker flow is long the favorite:** makers buy only 43.5% of 1–10¢ contracts but 56.5% of 90–99¢ ones (Table 10, p.26) — empirical support for asymmetric quoting.
- **The closing day is a different regime:** forecast error (MAE) declines smoothly then drops steeply on the closing day (Fig. 4, p.16), and on that day maker losses on ≤10¢ contracts approach taker-sized losses — the "Yogi Berra effect" (p.27, Fig. 8).
- **Bias is weakening over time:** ψ 2024 = 0.048*** but 2025 = 0.021* (Table 9, p.25). β=0.09 is a 2021–2025 average — recalibrate downward for current markets.
- **Model:** heterogeneous beliefs + self-selection into maker/taker + small over-estimation of low probabilities reproduces the FLB (§5, pp.31–36). The debias map is complement-consistent, µ(π*) + µ(1−π*) = 1 (p.35) — automatically Yes/No consistent for two-sided quoting. θ is weakly identified (0.80 correlation with σ across top fits, p.36).

**Quantitative anchors (used by our pricing/quoter):**

| Metric | Value | Source |
|---|---|---|
| Avg return, all contracts | −20% pre-fee | p.17 |
| Maker avg return | −9.64% | p.27 |
| Taker avg return | −31.46% | p.27 |
| Maker return on ≥50¢ | +2.6%, but only "some evidence" of significance; not replicated day-by-day; **pre-maker-fee era** | p.27, p.40, Fig. 8 |
| Maker return on <10¢ | negative & significant 5 of 6 pre-close days; ⚠ the "~−35%" figure is chart-derived (Fig. 6, p.28), not in the text | p.27–28 |
| Taker fee | $0.07·P(1−P)·n, **rounded up to the next cent per order** (effective 1.77% at 50¢×100; worse for small clips); f(P)/P rises as P falls | p.6, p.17 |
| Maker fee | **added after April 2025** — the sample (and the +2.6%) predates it | p.6 |
| Belief bias β | 0.09 (range 0.06–0.12) | p.36 |
| Maker match rate θ | 0.60 — **set, not estimated** ("moderately liquid") | p.36 |
| Belief dispersion σ | 0.107 | p.36 |

Debias map `π* = (P − 0.045) / 0.91` → 5¢→0.5%, 20¢→17%, 50¢→50%, 80¢→83%, 95¢→99.5%.

**Bias by category (Table 8, p.24):** ψ Crypto **0.058*** (largest)**, Other 0.053, Economics 0.034, Financials 0.032, Climate 0.031, Politics 0.022 (n.s.), Entertainment 0.020 (n.s.). ⚠ *earlier note claimed Crypto had smaller bias — inverted; and the paper never ranks categories by volume.* For a maker the right response is category-conditional debiasing, not category avoidance.

**Sample filters (p.9):** all anchors are estimated only on markets with volume ≥ $1,000, final spread ≤ 20¢, open ≥ 24h, hourly crypto/index markets excluded. β=0.09 doesn't necessarily transfer outside that envelope.

**Why the edge persists (§6, pp.40–41):** small absolute volumes (top-decile event avg $526k; mean trade $100, median $35 — Table 3 p.13, p.10), and risk: SD of maker ≥50¢ returns is **33%** against the 2.6% mean — the Sharpe context any Kelly sizing must respect. 2/3 of all contract observations are <10¢ or >90¢ (Table 2, p.12).

**Relevance to this project**
- **Quote ≥ ~15¢ only**, and treat the sub-50¢ *buy* side with an extra edge requirement — maker returns are negative there, not just below 10¢ (Fig. 6).
- **Maker-only is the right stance** (`post_only`) — but the documented maker edge is pre-maker-fee; with fees live, the γ·P·(1−P) spread widening should be **on by default**, modeled with per-order cent round-up.
- **The β=0.09 debias** is the `ViewBasedModel` coefficient's empirical basis (Phase 28) — should become category-conditional and drift-adjusted (2025 ψ ≈ 0.021).
- **Cancel-on-evidence is the maker edge** — motivates out-of-cycle quote fades when theo jumps, beyond the passive reprice threshold.
- ⚠ *the earlier "θ=0.60 ⇒ equilibrium spread ≈3–5¢" line had no basis in the paper (spreads are never quantified); dropped.*

---

## 2. Bawa (2025) — *The Mathematical Execution Behind Prediction Market Alpha*

Practitioner quant piece (Polymarket/Kalshi). Treats event contracts as
cash-or-nothing binaries and lays out the estimation + execution stack. High
signal for our *pricing* and *flow* components.

**Key ideas & formulas**
- **Edge = probability estimation, not vol.** `E[payoff] = P_true − P_market`; alpha ≈ 60% estimation, ~25% arbitrage, ~15% microstructure momentum (pp.3, 12–13).
- **Kelly sizing (pp.4–5):** binary-market form `f* = (P_true − P_market)/(1 − P_market)`; general `f* = (p·b − q)/b`; growth `G ≈ p·log(1+f·b) + q·log(1−f)`. Use **25–50% of full Kelly**. The famous table is a *bankroll-halving-before-doubling race*, not ruin: full Kelly 33%, half 11%, quarter <3% — and the paper flags these as illustrative under specific distributions. ⚠ *earlier note called this "<3% ruin".* Sizing recipe: `Position = Kelly_fraction × Confidence_Factor × Capital` (worked: 0.134 × 0.25 × $100k = $3,350).
- **Order-book imbalance (pp.7–9):**
  - `OBI = (Q_bid − Q_ask)/(Q_bid + Q_ask)` at **L1** (best-level depth); 5-level OBI "significant improvement in R²", 10-level marginal — empirical backing for the L2 leg of PLAN item 21.
  - ⚠ The R² = 0.65 is **Cont et al. (2014), an equities result** the paper calls "applicable to" prediction markets — not measured on them; a discussant (p.19) reports much lower R² in thin books. Thin Kalshi books likely similar.
  - `IR = Bid_vol/(Bid_vol + Ask_vol)`; **IR > 0.65 → up-move within 15–30 min at ~58% vs 50%** (p.8). It's a full template: enter on threshold, hold 15–30 min, exit on momentum reversal — the horizon defines the signal's decay window.
  - **VAMP** micro-price: `VAMP = (P_bid·Q_ask + P_ask·Q_bid)/(Q_bid + Q_ask)` (p.7).
- **Fair value = Bayesian aggregation:** `P_post = (w₁·P_polls + w₂·P_fund + w₃·P_market)/Σwᵢ`, weights tuned to minimize **Brier score** on history (p.9). Calibration facts (p.11): Polymarket ~90% accurate 30d out, ~94% hours before; systematic **2–3% overestimation bias**; methodology excludes >90%/<10% markets.
- **Binary Greeks (pp.2–3):** `P = e^(−rT)·N(φ·d₂)`; gamma is negative and `Γ ∝ 1/√T_remaining` — small probability shifts cause large late price swings. **Terminal taper** `Position(t) = Initial·√(T_remaining/T_initial)` is a continuous schedule, not a one-shot cut: $10,000 @30d → $4,830 @7d → $1,826 @1d (pp.10–11).
- **No-arbitrage across mutually-exclusive outcomes (pp.5–7):** if `Σ Pᵢ < 1`, buy all outcomes (worked: 0.38+0.33+0.27 = 0.98 → 2.04% gross, 0.54% net after 1.5% fees). Saguillo et al.: ~$40M realized arb on Polymarket (60% rebalancing / 40% combinatorial). Fees kill sub-0.5% edges; **simultaneous execution required — partial fills destroy the arb**. Kalshi position accountability: **25,000 contracts per strike per member** (CFTC filing, Nov 2024).
- **Info-incorporation speed (p.12):** sports <5 min, politics 15–60 min, economics 30–180 min.
- **Volume caveat (p.12):** Columbia (Nov 2025) — wash trading ≈14% of wallets, 20–60% of volume in some periods. Any volume-weighted signal (our scanner's 0.7·log-volume) inherits this noise.
- **Benchmarks (pp.12–15):** 15–25% annual, Sharpe 2.0–2.8, win rate 52–58%, edge 2–4%/trade, max DD 12–18%, 40–60% capital utilization, ~100ms book-latency target.

**Relevance to this project**
- **OBI / IR backs `FlowImbalanceGuard`** (Phase 26) and argues for a *directional* use (bounded fair-value lean or asymmetric size), not just spread widening.
- **VAMP** is shipped at L1 (PLAN item 21 tracks EMA/L2 deepening — the multi-level R² result above is its justification).
- **Fractional Kelly** should govern `quote_size` once quoting is stable; pair with the Brier-calibration logging the aggregation section implies.
- **Terminal-risk √-taper** → close-proximity size multiplier riding TheoGrid's time axis.
- **Sum-to-one monitoring** on multi-outcome events: renormalized fair values for free, arb detection as a bonus; defer multi-leg execution.

---

## 3. Berg & Proebsting (2009) — *Hanson's Automated Market Maker*

Derives implementation-grade formulae for an **LMSR** (Logarithmic Market
Scoring Rule) automated market maker. We run a CLOB maker, not an AMM, so this
is *contrast* — but it's the reference for principled **inventory-based
pricing**, and its transaction formulas verified exactly against the paper's
worked example (b = 463.232312, pp.56–59).

**Key formulas** (single security; `c` = max price, `P` = pre-trade price, `Q` = shares, `K` = cash; journal pages)
- **Price from inventory:** `pᵢ = c·e^{sᵢ/b} / Σ e^{sₖ/b}` (pp.48, 55) — prices sum to `c`, live strictly in (0, c). ⚠ *the earlier note's `C(q) = b·ln(Σ e^{q_i/b})` cost function is the standard Hanson/Chen–Pennock potential and is equivalent, but it never appears in this paper — the paper works entirely in per-transaction deltas (tables p.49, c-scaled p.55), which is the more useful form for a CLOB bot anyway.*
- **Price impact of a Q-contract fill:** `P' = c / (1 + (c/P − 1)·e^{−Q/b})` — a constant `Q/b` shift in **log-odds** space (p.49). Inverse: `Q = b·log(P'(c−P) / (P(c−P')))`.
- **Cost:** to move P→P′: `|K| = b·c·log((c−P)/(c−P′))`; of Q shares from P: `K = −b·c·log(P(e^{Q/b}−1)/c + 1)` (pp.49, 55).
- **Choosing b (pp.51, 55–56):** pick "a bet of size K should move P to P_upper" → `b = K / (c·log((c−P)/(c−P_upper)))`. Too small = wild swings; too large = static prices ("a vexing problem").
- **Worst-case loss:** `b·c·log(c/P)` per outcome — driven by the **lowest-priced outcome initially**; `b·ln(N)` is only the uniform-start special case ⚠ *(earlier note stated the special case as the general bound)*. Capped-capital bound: `loss = b·log((e^{K/b}−1)/P + 1) − K` (p.50).
- **Adjusting b live (pp.51–52, 54):** to change b without moving prices, reset the stock vector via `sᵢ = b_new·log(pᵢ) + X`. Microsoft's deployment rate-limited such changes.
- **Rounding / anti-cheat (pp.53–54, 57–59):** every rounding must favor the market maker — quantity **down** on purchases / **up** on sales, then cost **up** / **down** — or scripted round-trips become a cash pump (the worked example deliberately destroys $0.000084 per round trip). Store the exact stock vector and recompute prices; never accumulate price updates (float drift). A transaction fee larger than the money precision obviates price rounding.
- **Bundles (pp.50–53):** "bet against" = buy all other outcomes at `P_E = Σ_{i∈E} e^{sᵢ/b} / Σ e^{sₖ/b}`; conditional bets via `P = P_W/(P_W+P_L)`.

**Relevance to this project**
- **LMSR log-odds skew is the principled replacement for linear `skew_per_contract_cents`:** skew per contract should be constant in log-odds, which self-attenuates in cents near the price boundaries and can never push a quote out of (0, c). The `b` derivation gives a risk-budget-based way to set it (hit `max_position` ⇒ reservation price reaches P_upper).
- **The rounding rule has a direct CLOB analogue:** round our bid down and ask up — anything else systematically leaks the half-spread.
- The `b`-parameter (liquidity vs. subsidy) framing mirrors our `quote_size` vs. adverse-selection trade-off.

---

## 4. ToasterLightning (2026) — *Designing Prediction Markets* (LessWrong)

Accessible walkthrough of prediction-market plumbing: CLOBs, the market maker's
role, the bid–ask spread — then a first-principles derivation of a **CPMM**
(constant-product market maker, `y·n = k`, the Uniswap invariant).
⚠ *earlier note said the post builds toward LMSR — wrong: LMSR appears only in
the comment thread as a contrasting design (the author's footnote even retracts
the post's uniqueness claim; CPMM is one member of the CFMM family).*

**Key ideas**
- **CLOB** = post BUY/SELL orders, match takers against the best resting order; the maker continuously posts both sides and earns the spread; a CLOB dies when nobody posts both sides.
- **CPMM derivation:** every $1 mints a YES+NO pair (fully collateralized — Kalshi's own collateral model); price = `n/(y+n)`; trades preserve `y·n = k`.
- **Marginal ("bulk discount") pricing** — the post's pivot: filling a whole order at the pre-trade price undercharges; each marginal share should cost the *current* moving probability. The one genuinely reusable microstructure idea here for a CLOB maker.
- **Passive-curve bleed:** the AMM's liquidity provider "loses more of his initial liquidity the more confident a correct market is" — same bounded-subsidy story as Berg & Proebsting's loss bound; the intuition behind adverse-selection guards.

**Relevance to this project**
- The marginal-pricing point maps onto **laddering quote size across 2–3 price levels** instead of one flat quote: large sweeps pay progressively more, cutting adverse-selection cost per fill.
- CPMM's `P = n/(y+n)` is a closed-form convex inventory-skew alternative — log under the LMSR skew item, not separately.
- Otherwise foundational framing / onboarding vocabulary.

---

## 5. Chakraborty & Kearns (2011) — *Market Making and Mean Reversion* (EC'11)

The theory paper for the loss pattern runs 13–20 kept measuring. Studies a
ladder market maker against an *exogenous* price series (no dealer monopoly,
no stochastic assumptions for the main result) and derives an exact profit
identity. Studied 2026-07-11; page refs are the 7-page ACM version.

**Key results**
- **Theorem 2.1 (exact identity, any price path):** the maker that requotes a
  ladder around price every step (buy P−1 / sell P+1, depth Cₜ covering the
  next jump), with forced liquidation at T, earns exactly **(K − z²)/2** where
  `K = Σ|Pₜ − Pₜ₋₁|` (total path variation) and `z = P_T − P₀` (net move).
  Proof pairs each up-tick sell at p with the buy at p−1 on the next return;
  only |z| fills never pair, and liquidating them costs z(z−1)/2 (pp.3–4).
  Profit = fluctuation harvested minus net-move **squared**.
- **Theorem 3.1:** any random walk with even the *slightest* mean reversion
  toward P₀ ⇒ E[K] ≥ E[z²] ⇒ positive expected profit; an unbiased random
  walk yields exactly zero for every trading algorithm (martingale) (p.4).
  Mean reversion is *the* property that makes market making profitable.
- **Theorem 3.2 (OU process):** E[K] grows Ω(σT) linearly while
  E[z²] ≤ σ²/2γ + (µ−Q₀)² is bounded ⇒ profit grows **linearly in time**,
  with high probability — for arbitrarily weak reversion γ, given enough T
  (p.5). Analogous result for the Schwartz model (log-OU, p.6).
- **Granularity condition:** execution requires volatility comparable to the
  tick: profit grows only while limiting variance σ²/2γ ≳ tick² — too-fine
  ticks (or too-quiet books) mean orders rarely execute (p.5 remarks).
- **Trading frequency (§4, simulated):** requoting only every L steps barely
  hurts on a reverting series — L=40 still keeps >80% of the L=1 profit at
  T=1000, though profit variance rises sharply then stabilizes (Figs. 2–4).
- **MM vs stat arb framing (p.2):** the maker has *no directional view* and
  profits from non-directional volatility; stat arb (pairs trading) is the
  opposite — deliberate directional bets on convergence.

**Model limits (honest):** single exogenous price both sides can trade at;
guaranteed fills on touch (no queue competition), no market impact, no fees,
inventory unbounded, and **no adverse selection** — fills are mechanical
price-touches, not informed counterparties.

**Relevance to this project**
- **(K − z²)/2 is our attribution table.** entry_edge ≈ the K/2 harvest;
  the recurring **drift** line ≈ −z²/2 (run 13 −$0.42, run 19 −$0.59, run 20
  baseline −$0.48 — every losing run was a one-way z); exit_cost ≈ the
  liquidation term z(z−1)/2. Our one green session (2026-07-10 leg A, +$0.04)
  was the K-dominant case: oscillating books, small net move.
- **The adverse-selection omission matches demo reality better than
  Glosten–Milgrom:** measured picked-off fills ≈ 0/37 across runs — demo flow
  is *directional*, not informed. Our cost lives in the z² term, exactly the
  term this model prices.
- **It reduces profitability to a selection problem** — admit markets where
  K̂ ≫ ẑ² — and we can measure both directly from 1-minute candles (sum of
  |Δprice| vs net Δprice over a trailing window). PLAN item 67 builds this
  **reversion-score admission**; the two-sided-flow gate (item 65) is its
  crude flow-side proxy. Item 68 adds the K/z² split to attribution so every
  session tests the identity.
- **Validates, with theory, guards we built empirically:** the inventory
  brake (caps |z|, truncating the squared tail), passive wind-down (attacks
  the liquidation term), layered quoting (item 24 — the ladder is *required*
  by the identity to survive jumps), min-spread/liveness gates (the
  granularity condition; also a warning that sub-penny grids shrink per-loop
  harvest — item 69 adjacent), and rest timers (§4: requote frequency is not
  where the money is — deprioritizes L4 timer-teardown).

---

## Cross-cutting takeaways for our build

- **Stay a maker, quote the mid-range, lean toward the favorite side** (Bürgi: makers only profit ≥50¢ with significance above 70¢, winning maker flow is long favorites; sub-50¢ buys need an extra edge requirement, and the closing day is a regime of its own). ⚠ *"makers win" unqualified was wrong — makers average −9.64%; the game is being in the winning subset.*
- **Fees changed the game after the data ended:** the documented maker edge is pre-maker-fee. Fee-aware spread widening (γ·P·(1−P), ceil-to-cent per order) should be on by default (Bürgi p.6/p.17).
- **Micro-price + order-book imbalance** (Bawa) are the concrete upgrades to `FairValueEngine` (VAMP shipped; L2/EMA per item 21) and `FlowImbalanceGuard` (IR>0.65 as a bounded *directional* signal with a 15–30 min decay).
- **Edge is estimation, sized by fractional Kelly, verified by Brier logging** — with Bürgi's category-conditional, time-decaying β and the honest caveat that the Kelly race numbers are illustrative.
- **Inventory pricing in log-odds** (Berg & Proebsting) generalizes our linear skew; their rounding discipline (always favor the maker) applies verbatim to our quote rounding.
- **Profit = (K − z²)/2** (Chakraborty & Kearns): the maker's whole game is selecting reverting books and capping net moves — volatility is revenue, trend is cost squared. Admission should measure K̂ vs ẑ² directly (item 67); inventory caps and wind-down are the z-side defenses we already ship.
