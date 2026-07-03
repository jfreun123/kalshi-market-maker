# Research Papers & Notes

Reference papers on prediction-market microstructure, market-making, and pricing,
with study notes and their relevance to this project. The PDFs live alongside
this file (gitignored — binaries/copyrighted; keep locally). Cite these from
`PLAN.md` rather than duplicating the detail.

| File | Paper | Type |
|---|---|---|
| `burgi-deng-whelan-2026-makers-and-takers-kalshi.pdf` | Bürgi, Deng & Whelan (2026), *Makers and Takers: The Economics of the Kalshi Prediction Market* | Academic (UCD / GWU WP 2026-001) |
| `bawa-2025-prediction-market-alpha.pdf` | Bawa (2025), *The Mathematical Execution Behind Prediction Market Alpha* | Practitioner (Substack) |
| `berg-proebsting-2009-hanson-automated-market-maker.pdf` | Berg & Proebsting (2009), *Hanson's Automated Market Maker* | Academic (J. Prediction Markets) |
| `designing-prediction-markets-lesswrong-2026.pdf` | ToasterLightning (2026), *Designing Prediction Markets* | Blog (LessWrong) |

---

## 1. Bürgi, Deng & Whelan (2026) — *Makers and Takers: The Economics of the Kalshi Prediction Market*

The most directly relevant paper: the first systematic empirical study of Kalshi
pricing, on transaction-level data for 46,282 contracts / 12,403 events / 300k+
prices (2021–Apr 2025), plus a microstructure model of the maker/taker split.

**Key empirical findings**
- **Prices are informative** and get more accurate approaching close — but show a clear **favorite–longshot bias**.
- **Longshots lose badly:** contracts priced **< 10¢ lose > 60%** of stake. Contracts **> 50¢ earn a small, statistically significant positive return**. Average return across all contracts ≈ **−20%** pre-fee.
- **Makers beat Takers:** both show the favorite–longshot pattern, but it's worse for Takers (who hold more extreme beliefs and pay up to guarantee a fill). Kalshi's data flags maker vs. taker directly (no Lee–Ready guessing).
- **Model:** heterogeneous beliefs + self-selection into maker/taker + a small over-estimation of low probabilities reproduces the bias. Modeled Kalshi's pre-2025 fee (taker-charged, maker-free).

**Quantitative anchors (used by our pricing/quoter):**

| Metric | Value |
|---|---|
| Avg return, all contracts | −20% pre-fee |
| Maker avg return | −9.64% |
| Taker avg return | −31.46% |
| Maker return on ≥50¢ | **+2.6%** (stat. sig.) |
| Maker return on <10¢ | ~−35% |
| Taker fee | γ·P·(1−P), γ=0.07 (pre-Apr 2025) |
| Belief bias β | 0.09 (range 0.06–0.12) |
| Maker match rate θ | 0.60 |
| Belief dispersion σ | 0.107 |

Debias map `π* = (P − 0.045) / 0.91` → 5¢→0.5%, 20¢→17%, 50¢→50%, 80¢→83%, 95¢→99.5%.

**Relevance to this project**
- **Quote ≥ ~15¢ only.** Maker losses below ~10¢ are significantly negative — our scanner's `min_price_cents` gate and the quoter's price range should avoid deep longshots. (PLAN item 4 — align scanner band with risk gate.)
- **Maker-only is the right stance** (`post_only`) — makers structurally out-return takers; reinforces avoiding accidental taker fills (the whole point of the D1 clamp / cooldown work).
- **The β=0.09 debias** is exactly the `ViewBasedModel` belief-debiasing coefficient (Phase 28). This paper is its empirical basis.
- **θ=0.60 equilibrium spread** ≈ 3–5¢ at mid → the quoter's `min_spread_cents` floor.
- Prefer **Financials / Economics / Crypto** categories (larger volume, smaller bias) over sports/culture longshots.

---

## 2. Bawa (2025) — *The Mathematical Execution Behind Prediction Market Alpha*

Practitioner quant piece (Polymarket/Kalshi). Treats event contracts as
cash-or-nothing binaries and lays out the estimation + execution stack. High
signal for our *pricing* and *flow* components.

**Key ideas & formulas**
- **Edge = probability estimation, not vol.** `E[payoff] = P_true − P_market`; alpha comes from estimating `P_true` better than consensus (≈60% of edge), then arbitrage (~25%) and microstructure momentum (~15%).
- **Fractional Kelly sizing:** `f* = (P_true − P_market) / (1 − P_market)`; use **25–50%** of full Kelly (quarter-Kelly ≈ <3% ruin). Overconfidence in `P_true` destroys capital exponentially.
- **Order-book imbalance predicts short-term mid-moves:**
  - `OBI = (Q_bid − Q_ask) / (Q_bid + Q_ask)`; explains ~65% of short-interval price variance (Cont et al. 2014), subsumes trade-imbalance.
  - `IR = Bid_vol / (Bid_vol + Ask_vol)`; **IR > 0.65 predicts an up-move within 15–30 min (~58% vs 50%)**.
  - **VAMP** (volume-adjusted mid / micro-price): `VAMP = (P_bid·Q_ask + P_ask·Q_bid) / (Q_bid + Q_ask)` — weights price toward the heavier opposite side.
- **Fair value = Bayesian aggregation** of polls / fundamentals / market, weights tuned to minimize Brier score.
- **Terminal risk:** binary gamma ∝ `1/√T_remaining` — small prob shifts cause large late price swings; scale position by `√(T_remaining/T_initial)` (cut ~65% in the final week).
- **No-arbitrage across mutually-exclusive outcomes:** if `Σ P_i < 1`, buy all outcomes for a locked profit (Saguillo et al. documented ~$40M realized arb on Polymarket; 60% rebalancing, 40% combinatorial). Fees (1–2%) kill sub-0.5% edges.
- **Info-incorporation speed:** sports < 5 min, politics 15–60 min, economics 30–180 min — faster convergence = smaller alpha window.

**Relevance to this project**
- **OBI / IR is our `flow_imbalance` guard** (Phase 26) — this is the empirical backing and gives concrete thresholds (IR>0.65). Consider using IR as a directional *signal*, not just a spread-widener.
- **VAMP (micro-price)** is a better fair-value anchor than the raw mid for our `Quoter`/`FairValueEngine` — worth adopting.
- **Fractional Kelly** should govern `quote_size` / max-position when we move from fixed-size quoting to edge-scaled sizing.
- **Terminal-risk scaling** → the scanner's `max_days_to_close` and a future close-proximity size taper.
- **Cross-outcome arb** (`Σ P_i < 1`) is a distinct, lower-risk strategy we could add later (needs simultaneous fills — see execution constraints).

---

## 3. Berg & Proebsting (2009) — *Hanson's Automated Market Maker*

Derives the full formulae to implement an **LMSR** (Logarithmic Market Scoring
Rule) automated market maker from Hanson's market scoring rule — the canonical
"always-on" AMM. Covers buy/sell cost functions, rounding, and anti-cheat.

**Key ideas**
- Two matching mechanisms: **CDA** (continuous double auction / CLOB — what Kalshi and we use) vs **AMM** (LMSR — the market *is* the maker via a cost function).
- LMSR cost function `C(q) = b · ln(Σ e^{q_i / b})`; instantaneous price `p_i = e^{q_i/b} / Σ e^{q_j/b}`; the liquidity parameter **b** trades off price sensitivity vs. worst-case subsidy (max loss = `b · ln(n)`).
- Always provides liquidity (no counterparty needed) — solves the thin-book/wide-spread problem, at the cost of a bounded subsidy.

**Relevance to this project**
- We run a **CLOB maker**, not an LMSR AMM, so this is *contrast* rather than blueprint — but it's the reference for **inventory-based pricing**: LMSR's price moves deterministically with inventory, which is the principled version of our **inventory skew** (shift quotes vs. net position). Useful if we ever want a more rigorous skew curve than linear `skew_per_contract_cents`.
- The `b`-parameter framing (liquidity vs. subsidy) mirrors our `quote_size` vs. adverse-selection trade-off.

---

## 4. ToasterLightning (2026) — *Designing Prediction Markets* (LessWrong)

Accessible walkthrough of prediction-market plumbing: CLOBs, the role of the
market maker, the bid–ask spread, and pricing (building toward LMSR). Good
onboarding / shared-vocabulary reference; little new for our implementation.

**Key ideas**
- **CLOB** = post BUY/SELL orders, match takers against the best resting order; bid–ask spread = gap between best bid and best ask.
- **Market maker** continuously posts both sides and profits from the spread ("crossing the spread" / "bazaar flipping"); needed because a CLOB dies when nobody posts both sides.
- Fair-price problem for the MM leads into inventory-based/LMSR pricing (see paper 3).

**Relevance to this project**
- Foundational framing of exactly what our bot *is* (a CLOB market maker earning the spread). No direct action items; useful for orienting new contributors and for the bid/ask/spread vocabulary used throughout `PLAN.md` and the code.

---

## Cross-cutting takeaways for our build

- **Stay a maker, quote the mid-range, avoid longshots** (Bürgi: makers win, <10¢ loses; Bawa: maker rebates + spread). Directly supports `post_only`, the D1 clamp, and the scanner price gate.
- **Micro-price + order-book imbalance** (Bawa) are concrete upgrades to `FairValueEngine` (use VAMP not raw mid) and `FlowImbalanceGuard` (IR>0.65 as a directional signal).
- **Edge is estimation, sized by fractional Kelly** — the north star once quoting is stable: a sharper `P_true` (view-based model, β=0.09 debias per Bürgi) scaled by quarter-Kelly.
- **Inventory-based pricing** (LMSR) is the principled generalization of our linear inventory skew if we want to harden it.
