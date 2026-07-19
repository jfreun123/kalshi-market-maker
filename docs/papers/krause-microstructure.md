# Krause — An Introduction to Market Microstructure Theory (2005 draft)

Full-book study notes (Bath lecture-notes textbook, 322pp), distilled for this
project. Text-page references throughout (text page = PDF page − 20). The
book's dealer-market results apply verbatim to limit-order liquidity
suppliers (its own footnote, p.154) — i.e. to us. Related notes:
[README.md](README.md) §1 (Palumbo), §4 (Bürgi-Deng-Whelan FLB), §5
(Chakraborty-Kearns), §6 (ND-HFTT).

The one-paragraph version: our guards implement, heuristically, what chapters
2–4 derive formally — and the formal versions come with *calibratable
parameters* our analytics JSONL can estimate. Chapter 5's estimators got
easier for us than for the literature because Kalshi's public tape carries
the exact taker side. Chapters 6–7 give the market-*selection* theory: when
making is viable at all, and how much of an illiquid contract's apparent
mispricing is rational friction compensation.

## Ch.2 — Auction markets (Kyle): the adverse-selection tax

- **Kyle λ** (p.33–36): price impact per unit net order flow. λ ≈
  ½·√(Σ₀/σ_u²) — rises with fundamental uncertainty Σ₀, falls with noise-flow
  variance σ_u². **λ is the adverse-selection tax per contract**; estimate a
  live λ̂ by regressing mid changes on signed net taker flow in a rolling
  window. Spread floors should scale with √(event uncertainty / noise
  volume): widen near announcements and when volume dries up, *before* any
  imbalance appears.
- **Two informed-flow regimes** (single vs many insiders, p.36–48):
  a monopolist insider hides — small persistent one-sided flow, steady
  drift, *stable* λ̂; danger lasts all session, stay wide. Competing
  insiders race — one violent burst + price jump, then λ̂ collapses and
  residual information is spent; **after the burst it is theoretically safe
  to re-tighten**. Discriminate on the λ̂ trajectory after a jump.
- **Market breakdown bound** (Spiegel-Subrahmanyam, p.54): a quoting
  equilibrium exists only while the informed share is small relative to the
  hedging/noise base (M < ¼·N·z²·Σ₀·σ_w²). Past the bound there is NO price
  at which liquidity provision is rational — the theoretically correct
  action is a **hard halt, not a wider spread**. (Justifies item 51's panic
  pull and our halt-first safety stance; echoes Palumbo: viability requires
  a noise-flow subsidy.)
- **Volume classifies information precision** (Blume-Easley-O'Hara,
  p.64–70): price change measures the belief move; volume measures its
  precision. Practical 2-D classifier per window on (|Δmid|, volume):
  drift + high volume = precise info, follow it, don't fade; drift + thin
  volume = imprecise, partially fadeable; **high volume with no net price
  change = hedging noise, the farmable regime — tighten**. Weight
  drift-based fv updates by volume confirmation (upgrade to item 60a).
- **Cross-impact is symmetric across correlated assets**
  (Caballé-Krishnan, p.59–64): signed flow in one leg of an event family is
  fair-value information for the others; imbalance confirmed by correlated
  legs is far more likely informed.
- Timing patterns (p.70–71): after non-trading periods information
  accumulates but hedging flow doesn't → first flow after opens/halts/news
  gaps is disproportionately informed — **start wide, tighten as noise
  participation confirms**. Volume clusters are noise-rich per trade
  (Admati-Pfleiderer) *if* two-sided — combine with the volume classifier.

## Ch.3 — Dealer markets: inventory and information (the priority chapter)

### Inventory family (symmetric information)
- **Quote-skew steers inventory** (Garman framework, p.77–79): choose
  quotes so bid/ask arrival intensities λ_b, λ_a differ; our LMSR skew is
  this. The needed input every model shares: the **empirical fill-intensity
  curve λ(δ) — fills/hour vs quoted distance from fair — which we log but
  have never fitted.** Fitting it unlocks Ho-Stoll/Avellaneda-Stoikov-style
  optimal spreads.
- **Stoll 1978** (p.79–86): half-spread cost of taking a trade =
  z·σ²·(½Q ± I) — spread s = zσ²Q² is inventory-*independent*; inventory
  shifts the quote *pair* linearly (what our skew does). For a binary at
  price p the per-contract variance is **p(100−p)**: inventory half-spread
  is widest at 50c and →0 at the extremes — where the FLB/informed-flow
  evidence (BDW) says adverse selection dominates instead. The
  decomposition makes the near-extremes stance explicit: the AS term must
  dominate there, not the inventory term. Also: spread should scale with
  quoted size Q (we don't).
- **Competition** (Ho-Stoll 1983, p.87–93): second-price structure — the
  best-inventory dealer wins at the runner-up's reservation price; profit
  per fill = inventory advantage. With >3 dealers the *inside* spread can
  be tighter than any one dealer's reservation spread. Losing the inside on
  the side our skew concedes is the model working, not a bug. Testable:
  inside-spread mean reversion after large prints (inventory signature).
- **Monopolist** (Ho-Stoll 1981, p.93–99): optimal fee = monopoly term
  α/(2β) (from the fill-intensity curve) + risk term growing with z, σ², Q
  and **remaining horizon τ**. Two new levers: elasticity-aware spreads in
  markets where we're effectively sole quoter, and a **horizon term —
  slow-unwind markets deserve structurally wider quotes** (supports item 70
  max-hold; testable: holding time vs markout loss).

### Information family (informed + uninformed flow)
- **Bagehot identity** (p.100–102): MM always loses to informed flow and
  must recoup from benign flow — the spread IS the cross-subsidy. Breakeven:
  half-spread ≥ maker fee + (informed share γI) × E[markout loss]. Our
  markout curves measure the loss term directly → **the adverse-selection
  component of the spread floor can be set empirically per market class.**
- **Glosten-Milgrom regret-free quotes** (p.106–107): quote each side at
  the value *conditional on being hit* — pa = E[v | next order is a buy].
  Implementable as a per-fill fair-value bump (every taker print shifts fv
  by a calibrated amount ∝ γI). Our flow lean is the heuristic version.
- **The classic discriminator** (p.107): information models ⇒ trade-price
  changes are a martingale (≈zero autocorrelation); inventory models ⇒
  negative autocorrelation. **Per-market tape autocorrelation is cheap and
  tells us which regime we're in: negative = bounce-harvestable, safe to
  tighten; zero/positive = informed, respect the floor.** (Same object as
  the K/z² split, independently derived.)
- **Spread-volume law** (p.110): E[s²] ≤ ξ·Var[v]/T — more trades reveal
  information faster, so spreads shrink with activity; γI spikes right
  after information events → **pre-event widening is correct, not
  reactive widening** (our toxic-event problem, formalized).
- **Glosten 1989 breakdown** (p.112–114): a competitive quoter should exit
  when toxicity exceeds what the spread covers; running losses to "learn
  the flow" is rational only for a durable monopolist. Validates
  halt+cancel. Testable: when our halt fires, does the rest of the book
  also empty (real breakdown) or stay (we over-trigger)?
- **Admati-Pfleiderer asymmetric quoting** (p.114–116): with inventory on,
  quote tight *only* on the unwinding side and near-pull the other — fills
  on the wide side are near-surely informed and should update fv strongly
  (**weight fills by execution distance from fair**, not equally). Item 64's
  asymmetric unwind pricing already ships the first half; the
  fill-informativeness weighting is the missing second half.
- **Multi-asset dealer** (Gehrig-Jackson, p.116–121): adjacent strikes are
  substitutes — cross-strike competition caps any per-strike monopoly fee.
  For the portfolio phases: the skew term in market j should use the
  **portfolio covariance-weighted exposure (Σ·I)_j** — holding YES in a
  correlated market should skew this market's quotes too. A genuinely new
  lever for Phases 21–25.

## Ch.4 — Limit order markets (our venue form)

- **Why spreads floor out without any adverse selection** (Cohen et al.,
  p.127–130): execution probability jumps discontinuously at the touch
  (φ < 1 just inside, = 1 crossing), so a floor spread survives unlimited
  competition; the jump is bigger when trade arrival λ is small — **thin
  event markets have structurally wide spreads, no informed flow needed.**
  Second independent floor (FKK below): s₁ = ⌈c·Δt/d⌉ — waiting cost per
  arrival interval in ticks. Our per-market spread floor should scale with
  expected time-to-fill, not be a global constant.
- **Queue value is quantified** (Cohen: joining a queue costs your flow
  share; FKK 4.34: back-of-queue adds ≥ 2(1+θ(2−θ))Δt expected wait). The
  item-42b reprice rule has an exact form: reprice from rung i to j iff
  (ticks gained)·d > c·(t̂_new − t̂_current), with waiting times growing
  **geometrically** in depth (ratio θ/(1−θ)) — the functional form to fit
  per (ticks-from-inside, queue rank) from our fill logs.
- **Parlour 1998 — one-sided flow can be purely mechanical** (p.130–137):
  with ZERO information asymmetry, buys follow buys because each buy thins
  the ask queue and pushes marginal traders into market buys. Same-side
  runs are predicted by thin-ask/thick-bid book states. **Consequences:
  condition markouts on book imbalance at fill time to separate Parlour
  continuation from informed flow before the one-sided-flow logic widens or
  halts; add opposite-side depth to the fill-hazard model; book imbalance
  is an early-warning feature.** (Directly attacks our #1 open problem.)
- **FKK 2005 — the spread ladder** (p.137–150): equilibrium rungs s₁<s₂<…
  with spacing ⌈2(θ/(1−θ))^{i−1}·Δt·c/d⌉ where θ = patient-trader fraction
  (measurable as 1 − taker fraction in our tape). Item 24's layers should
  sit at these rungs; deep layers only pay in impatient-heavy (low-θ)
  markets. Rest timers get principled scales from t(s₁)=Δt and the duration
  law D_i — calibrate from observed inter-trade duration, not constants.
  **Kalshi's fat 1c tick violates FKK's no-queueing condition → we are in
  the queueing regime: time priority is a real asset**, amend-first is right
  exactly when it preserves priority, and penny-wars are structurally rare.
- **Informed traders use limit orders too** (Brown-Zhang, p.150–151):
  passive counterpart flow is not uninformed. Being joined, undercut, or
  bunched-on at a level is a signal — extend markouts to book *events*
  (post-undercut, post-large-join), not only our fills.

## Ch.5 — Estimators (all directly implementable: our tape has exact taker side)

Priority-ranked; the literature's covariance contortions exist because
equities lacked trade signs — we can estimate the primitives directly.

1. **γ/δ spread decomposition** (Stoll 1989, p.155–160). γ̂ = P(next tape
   trade flips side); δ̂ from E[Δp | repeat] = −δs vs E[Δp | flip] = (1−δ)s.
   Component shares: adverse selection s_A = 1−2(γ̂−δ̂), inventory
   s_I = 2γ̂−1, order processing s_O = 1−2δ̂; realized spread s_r = 2(γ−δ)s
   is exactly our next-trade markout. A few group-bys on the tape → a
   three-way spread decomposition per series/session. Pool thin markets
   (<100 prints = noise).
2. **Transitory-variance split** (Krause 2003, p.164–166): from mid-price
   bars, α̂ = −Cov[ΔP,ΔP₋₁], σ̂² = Var[ΔP] − 2α̂; report transitory share
   2α̂/(2α̂+σ̂²), overidentification check Cov[ΔP²,ΔP²₋₁] ≈ 2α̂². This is
   K/z² re-derived from quote logs alone — **works on scanner markets we
   never quoted**; divergence from the fill-based split flags pick-off
   contamination.
3. **PIN** (Easley et al., p.161–164): three-component Poisson mixture over
   per-period (buys, sells) counts → P(any trade is informed) = αμ/(αμ+2ε).
   Our exact trade signs remove the estimator's main known bias. Use
   per-series, event-window periods (constant-arrival is the weak
   assumption). Uses: market screen + spread-floor scaling; cross-check:
   high-PIN markets should show worse maker markouts.
4. **Roll proxy** s = 2√(−Cov[Δp,Δp₋₁]) — free effective-spread estimate
   for unquoted scanner markets; report missing when autocovariance > 0.

## Ch.6–7 — When making is viable, and friction-aware pricing

- **Grossman-Miller immediacy** (p.171–178): maker profit = bridging a
  temporary imbalance until offsetting flow arrives. Equilibrium maker
  count M = |imbalance|·√(z·Var/2C) − n₁. **Viability needs recurring
  imbalance, price risk worth paying to shed, AND a credible future
  offsetting-flow population. Persistent one-way flow means the profit
  mechanism does not exist — you're not supplying immediacy, you're
  accumulating a position.** The formal version of items 65/67's admission
  gates and the selection principle. Crowding kills edge quadratically.
- **Yavas** (p.178–189): quoting pays when participant valuations are
  dispersed relative to the spread and search/waiting is costly; markets
  where everyone agrees on the probability leave nothing between
  reservation prices (another lens on unquotable tight demo books).
- **Amihud-Mendelson amortized spread** (p.194–198): required return rises
  by s/T (spread over holding horizon); prices embed the PV of future
  trading costs, concavely, with a clientele effect (long-horizon holders
  sort into wide-spread assets → structurally low repeat flow there).
  **An illiquid long-dated contract SHOULD trade below model-fair — that
  discount is friction compensation, not edge; don't quote it away and
  don't let the scanner read it as mispricing.**
- **Adverse-selection premium** (Easley/O'Hara, p.198–202): uninformed
  holders demand a discount that is *largest in low-λ opaque markets* (few
  insiders, unrevealing prices — niche events). Noise-heavy flow is good
  for a maker; insider-thin-opaque is the worst cell — wide, skewed, or
  skip.
- **Price impact and sizing** (p.202–205): optimal position ∝
  edge/(risk + 2·impact); friction compensation annualizes brutally at
  short horizons (~12%/yr at 1-month) — near-expiry "cheapness" is often
  just this. Impact-aware per-market size caps, not one global size.
- **Liquidity risk is correlated** (Acharya-Pedersen, p.205–208): exit
  costs across the whole book spike together exactly when marks move
  against us — the dominant priced term. Portfolio halts should treat
  correlated liquidity evaporation as one factor; inventory held into
  event windows deserves an extra skew premium.

## Consolidated: what to do with this

**Already built and now theory-confirmed**: LMSR inventory skew (Stoll),
flow lean (Glosten-Milgrom, heuristic form), halt-on-toxicity + cancel-on-
halt (Glosten breakdown; Spiegel-Subrahmanyam existence bound), asymmetric
unwind pricing (Admati-Pfleiderer, half of it), two-sided-flow + reversion
admission gates (Grossman-Miller viability), spread-vs-activity intuition
(GM volume law), maker-fee widening (order-processing component).

**New implementable levers, roughly ranked by value/effort**:
1. γ/δ spread decomposition + tape autocorrelation discriminator (script
   work only; generalizes markout; tells us when tightening is safe).
2. Volume-precision classifier on (|Δmid|, volume) windows; weight fv drift
   updates by volume confirmation (item 60a upgrade).
3. Markout conditioning on book imbalance at fill (separate Parlour
   mechanical runs from informed flow — attacks the #1 loss channel).
4. Empirical fill-intensity curve λ(δ) per market → Ho-Stoll optimal
   half-spreads, monopoly fee α/(2β) in thin books, principled rest timers
   and layer rungs (feeds items 24/42b/60b).
5. Live λ̂ (impact regression) with regime classification (stable vs
   collapsing after jumps) → dynamic spread floor.
6. Per-fill regret-free fv bump sized by γI/PIN; fill-informativeness
   weighting by execution distance from fair.
7. PIN per series as market screen + floor scaler.
8. Horizon term in the spread (expected unwind time); impact-aware size
   caps; portfolio covariance-weighted skew (Σ·I)_j for Phases 21–25.

**Testable immediately on existing logs**: tape autocorrelation per market ·
γ/δ decomposition · transitory-variance split vs K/z² agreement ·
time-to-fill vs (distance, queue rank, trade rate) regression (FKK check) ·
holding-time vs markout-loss correlation (horizon term) · book-imbalance-
conditioned markouts (Parlour) · post-halt book-emptying check (Glosten).
