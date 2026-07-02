# Kalshi Exchange API Reference (for C++ Market Maker)

> Compiled from https://docs.kalshi.com and **re-verified against the live docs on 2026-07-02**. Covers the Predictions (event-contract) Trade API: REST, WebSocket, and FIX pointers. Raw specs for codegen: `https://docs.kalshi.com/openapi.yaml` (REST) and `https://docs.kalshi.com/asyncapi.yaml` (WebSocket). Machine-readable page index: `https://docs.kalshi.com/llms.txt`.

## Conformance notes for our implementation (2026-07-02 audit)

Discrepancies found between our C++ code and the verified live API. Fix separately from this doc:

1. **🔴 `place_order` reads the wrong response field.** `rest_client.cpp` (`place_order`, ~line 357) reads `fill_count_fp` from the Create Order V2 response, but that endpoint returns **`fill_count`** (fixed-point string, **no `_fp` suffix**; likewise `remaining_count`). `at("fill_count_fp")` throws `json::out_of_range` on a real response — every live order placement breaks. Our tests pass only because `paper_transport.cpp` emits the same wrong name. **Careful:** this is endpoint-specific — the **Get Orders** order resource (used by `parse_order`, ~line 179) genuinely *does* use `initial_count_fp`/`fill_count_fp`/`remaining_count_fp` (verified), so that call is **correct**; only the create-order response path is wrong. See §5/§6. **Highest priority — live order path.**
2. **🟠 We send `use_yes_price: true` on `orderbook_delta` subscribe.** That parameter is **not in the current channel docs** (§8.4). If the server ignores it, fine; if it honors it, NO-side levels arrive on the yes scale and our `complement()` (`1 − price`) inverts the ask. Verify the NO-side price scale against a live demo capture; drop the flag unless a capture requires it.
3. **🟢 Validated, no change needed.** Auth signing (salt = digest length, §2), `fee_cost` = total-dollars-per-fill (§8.5, matches the fee-PnL work), `outcome_side` fill dispatch (§4.3), REST orderbook `orderbook_fp`/bids-only (§4.4), delta-as-signed-increment (§8.4).

---

## 1. Environments & Base URLs

Production and demo credentials are **not** interchangeable.

| Environment | REST (recommended) | REST (also supported) |
|---|---|---|
| Production | `https://external-api.kalshi.com/trade-api/v2` | `https://api.elections.kalshi.com/trade-api/v2` |
| Demo | `https://external-api.demo.kalshi.co/trade-api/v2` | `https://demo-api.kalshi.co/trade-api/v2` |

| Environment | WebSocket (recommended) | WebSocket (also supported) |
|---|---|---|
| Production | `wss://external-api-ws.kalshi.com/trade-api/ws/v2` | `wss://api.elections.kalshi.com/trade-api/ws/v2` |
| Demo | `wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2` | `wss://demo-api.kalshi.co/trade-api/ws/v2` |

Notes:
- The `external-api` hosts are dedicated to external API traders and are the recommended hosts.
- Despite the `elections` subdomain, production covers **all** Kalshi markets.

## 2. Authentication (RSA-PSS request signing)

Every authenticated request (REST and the WebSocket handshake) carries three headers:

| Header | Value |
|---|---|
| `KALSHI-ACCESS-KEY` | API Key ID (UUID) |
| `KALSHI-ACCESS-TIMESTAMP` | Request timestamp in **milliseconds** since epoch (string) |
| `KALSHI-ACCESS-SIGNATURE` | Base64 RSA-PSS signature (see below) |

Signature construction:

```
message   = timestamp_ms_string + HTTP_METHOD + path
signature = base64( RSA_PSS_sign(SHA256, message, private_key) )
```

- Padding: **RSA-PSS**, MGF1 with SHA-256, salt length = digest length (32 bytes).
- Hash: SHA-256.
- `path` is the full path from the API root, e.g. `/trade-api/v2/portfolio/orders`.
- **Strip query parameters before signing.** For `.../portfolio/orders?limit=5`, sign `/trade-api/v2/portfolio/orders`. The hostname is never part of the signed payload.
- Keys are RSA private keys generated at https://kalshi.com/account/profile (or via the API-keys endpoints for Premier/Market Maker tiers, which allow supplying your own public key).

C++ implementation note: OpenSSL `EVP_DigestSign*` with `EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PSS_PADDING)` and `EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, RSA_PSS_SALTLEN_DIGEST)` matches the required scheme.

## 3. Rate Limits & Tiers

Token-bucket model. Every authenticated request costs **tokens** (default **10**; a few ops are cheaper — order cancels cost 2, single-order reads, quote create/cancel, and MVE-collection lookup are also below default). Effective request rate = `budget ÷ cost`.

Two **independent** per-second budgets:

| Bucket | Covers |
|---|---|
| Read | `GET` endpoints and anything not routed elsewhere |
| Write | Order placement, amends, cancels, order groups, RFQ quote flow |

Tiers (tokens/sec per bucket):

| Tier | Read | Write |
|---|---|---|
| Basic | 200 | 100 |
| Advanced | 300 | 300 |
| Expert | 600 | 600 |
| Premier | 1,000 | 1,000 |
| Paragon | 2,000 | 2,000 |
| Prime | 4,000 | 4,000 |
| Prestige | 6,000 | 8,000 |

- **Batch endpoints do not save tokens**: each item is billed individually (25 creates = 250 tokens; 25 cancels = 25 × 2 = 50 tokens). Max batch size scales with the tier's write budget.
- **Bursting**: Basic-tier Write holds only **one second** of budget (no headroom). Advanced-and-up Read and Premier-and-up Write hold **two seconds** and permit twice the per-second budget in a single burst. It refills continuously; idle time builds burst headroom.
- Rate-limited requests return `429` with body `{"error": "too many requests"}`. **No `Retry-After` or `X-RateLimit-*` headers** — use exponential backoff; the next request succeeds as soon as the bucket covers its cost.

## 4. Data Conventions (critical)

### 4.1 Fixed-point strings everywhere

Kalshi migrated to fixed-point string representations across all APIs:

- **Prices**: `*_dollars` fields — fixed-point dollar strings, up to 4 decimal places (e.g. `"0.1200"` = $0.12). Intermediate fee math can reach 6 decimals. Request/response schema type `FixedPointDollars` allows up to 6 decimals of precision.
- **Contract counts**: `*_fp` fields — fixed-point strings, 0–2 decimals accepted on input, always 2 decimals on output (e.g. `"10.00"`). Minimum granularity 0.01 contracts (fractional contracts exist on some markets — you will see fractional values in fills even if you never send them).
- Legacy integer count / cent-price fields are deprecated; when both are supplied they must match.
- Recommended integer-arithmetic strategy: parse `_fp` values ×100 into `int64` ("centi-contracts") and `_dollars` ×10000 into `int64` ("deci-cent*10" i.e. hundredths of a cent). Never use floating point for prices/counts.

### 4.2 Price level structures (tick sizes)

Per-market field `price_level_structure` on Market responses, with `price_ranges` giving exact intervals:

| Structure | Ranges | Tick |
|---|---|---|
| `linear_cent` | $0.00–$1.00 | $0.01 |
| `tapered_deci_cent` | $0.00–$0.10 and $0.90–$1.00 | $0.001 |
|  | $0.10–$0.90 | $0.01 |
| `deci_cent` | $0.00–$1.00 | $0.001 |

Quote generation must respect the active structure per market.

### 4.3 Order direction: `outcome_side` / `book_side`

Direction on every Order, Fill, and Trade is expressed by two equivalent fields:

- `outcome_side` ∈ {`yes`, `no`} — which outcome you're positioned long.
- `book_side` ∈ {`bid`, `ask`} — same bit; **`bid ≡ yes`, `ask ≡ no`, always**.

Mapping from legacy `(action, side)`:

| Legacy action | Legacy side | outcome_side | book_side |
|---|---|---|---|
| buy | yes | yes | bid |
| sell | no | yes | bid |
| buy | no | no | ask |
| sell | yes | no | ask |

- Direction does **not** change price: an order at price `p` with `outcome_side=no` matches an order at the same `p` with `outcome_side=yes`.
- Public trades use `taker_outcome_side` / `taker_book_side`.
- Legacy fields are deprecated, removed **no earlier than May 28, 2026** (per the live `order_direction` doc): `action`/`side` (Order, Fill), `is_yes` (Order WS), `purchased_side` (Fill WS), `taker_side` (Trade REST + WS). New code should read only the new fields. Note: some WS example payloads in the docs still *show* the legacy fields, so they may still be present — but do not depend on them.

### 4.4 Orderbook semantics: bids only

The book returns **bids only** for both YES and NO. In a binary market:

- YES BID at X ≡ NO ASK at (1.00 − X)
- NO BID at Y ≡ YES ASK at (1.00 − Y)

Spread math: best YES ask = 1.00 − (best NO bid); YES spread = best YES ask − best YES bid.

REST orderbook (`GET /markets/{ticker}/orderbook`) response:

```json
{
  "orderbook_fp": {
    "yes_dollars": [["0.4100","10.00"], ["0.4200","13.00"]],
    "no_dollars":  [["0.5500","20.00"], ["0.5600","17.00"]]
  }
}
```

- Each level is `[price_dollars, count_fp]` (both strings).
- Arrays sorted **ascending by price**; **best bid is the last element**.
- No auth required for this endpoint. There is also `GET` multiple-market orderbooks in one request.

## 5. REST API Catalog

All paths relative to `/trade-api/v2`. Default cost 10 tokens unless noted.

### Exchange
- `GET /exchange/status` — exchange/trading active flags.
- `GET /exchange/schedule` — trading hours.
- `GET /exchange/announcements` — exchange-wide announcements.
- `GET /exchange/user_data_timestamp` — approximate freshness of read-API state. There is a short delay before exchange events appear in REST reads; combine write responses + WebSocket for the accurate view.
- Series fee changes endpoint for fee schedule updates.

### Market data (public)
- `GET /markets` — filterable by `series_ticker`, `event_ticker`, `status` (`unopened`, `open`, `closed`, `settled`; one at a time), timestamps. Paginated.
- `GET /markets/{ticker}` — single market (contains `price_level_structure`, `price_ranges`, times, `settlement_timer_seconds`, etc.).
- `GET /markets/{ticker}/orderbook` — see §4.4.
- Multiple-orderbooks endpoint — batch book fetch in one request.
- `GET /markets/trades` — public trades, paginated.
- `GET /markets/{ticker}/candlesticks` and batch variant — periods 1m / 1h / 1d.
- `GET /events`, `GET /events/{ticker}`, event candlesticks/metadata; multivariate events via `GET /events/multivariate`.
- `GET /series`, `GET /series/{ticker}`.

### Orders — V2 (preferred; legacy `/portfolio/orders` deprecated no earlier than May 6, 2026)
- `POST /portfolio/events/orders` — **Create Order (V2)**. See §6.
- Cancel (V2) — returns `{order_id, client_order_id, reduced_by}`. Cancel cost: 2 tokens.
- Amend (V2) — update price and/or max fillable count (`count` = filled + desired resting remaining).
- Decrease (V2) — exactly one of `reduce_by` / `reduce_to`. Only way to reduce quantity; cancel ≡ decrease-to-zero.
- Batch create / batch cancel (V2) — batch size scales with write budget; per-item token billing.
- `GET /portfolio/orders` (filter by status: `resting`, `canceled`, `executed`), `GET` single order (cheap), order queue position (single and all-resting-orders variants). Queue position = contracts ahead at price-time priority.
- Per-user cap: **200,000 open orders**.

### Order groups (MM kill switch — see §7)
- Create / get / list / delete / reset / trigger group; update group limit.

### Portfolio
- `GET /portfolio/balance` — balance + portfolio value (cents).
- `GET /portfolio/positions` — filter by non-zero `position` / `total_traded`.
- `GET /portfolio/fills` — fills since historical cutoff (older via `/historical/fills`).
- `GET /portfolio/settlements`, deposits, withdrawals.
- Subaccounts (institutions/MMs only): create (max 32), balances, transfers, netting settings. `subaccount` 0 = primary.

### Historical (archived past the cutoff)
- `GET /historical/...`: cutoff timestamps, markets, market candlesticks, orders, fills, trades. Live endpoints only serve data after the cutoff.

### RFQ / Communications
- Create/get/delete RFQs (max 100 open), create/confirm/accept/delete quotes, communications ID. Quote create/cancel are below default token cost.

### Multivariate (combo) collections
- Get collections, get collection, `PUT` create-market-in-collection (must be hit before trading a combo market; 5,000 creations/week). Lookup endpoints are deprecated in favor of RFQs.

### Misc
- Account API limits (`GET`), non-default endpoint costs list, milestones, structured targets, live data (sports play-by-play), incentives, FCM endpoints (rare).

## 6. Create Order (V2) — `POST /portfolio/events/orders`

Request (required unless noted):

| Field | Type | Notes |
|---|---|---|
| `ticker` | string | Market ticker |
| `client_order_id` | string | Your idempotency/correlation ID; echoed on WS deltas you cause |
| `side` | `bid` \| `ask` | **YES-leg only**: `bid` = buy YES, `ask` = sell YES (≡ buy NO at 1−p) |
| `count` | FixedPointCount | e.g. `"10.00"` |
| `price` | FixedPointDollars | e.g. `"0.5600"` |
| `time_in_force` | enum | `fill_or_kill`, `good_till_canceled`, `immediate_or_cancel`. `GTT` is not a valid API value — for expiring orders use `good_till_canceled` + `expiration_time` |
| `self_trade_prevention_type` | enum | `taker_at_cross` (cancel incoming taker; partials already matched execute) or `maker` (cancel resting maker, continue matching) |
| `expiration_time` (opt) | int64 | Unix **seconds**; not combinable with IOC |
| `post_only` (opt) | bool | Reject rather than cross |
| `cancel_order_on_pause` (opt) | bool | Auto-cancel on trading/exchange pause (see §9) |
| `reduce_only` (opt) | bool | Cap placeable count by current position |
| `subaccount` (opt) | int | 0 = primary |
| `order_group_id` (opt) | string | Attach to a risk group (§7) |
| `exchange_index` (opt) | int | Exchange shard; default `0`, use `-1` for auto-routing |

Response (`201`):

| Field | Notes |
|---|---|
| `order_id`, `client_order_id` | strings |
| `fill_count` | Contracts filled immediately on placement |
| `remaining_count` | After placement; for IOC, final state after unfilled cancel |
| `average_fill_price` | VWAP; present only if `fill_count > 0` |
| `average_fee_paid` | Per-contract VWAP fee; present only if `fill_count > 0` |
| `ts_ms` | Matching-engine timestamp, epoch ms |

> ⚠️ **Naming trap — verified against the live example response.** On *this* endpoint the count/price fields are `fill_count`, `remaining_count`, `average_fill_price`, `average_fee_paid` — fixed-point **strings with NO `_fp`/`_dollars` suffix** (e.g. `{"fill_count":"0.00","remaining_count":"10.00"}`). This is inconsistent with the WS/orderbook fields that *do* carry `_fp` (`count_fp`, `delta_fp`). Read `fill_count`, not `fill_count_fp`, from the create-order response.

Errors: `400`, `401`, `409` (conflict), `429`, `500` with `{code, message, details, service}`.

## 7. Order Groups (risk kill switch)

Order groups are the exchange-side risk primitive for market makers:

- A group has a **contracts limit measured over a rolling 15-second window** of matched contracts.
- When the limit is hit, **all orders in the group are cancelled** and no new orders can be placed into it until you call **reset** (zeroes the matched-contracts counter).
- **Trigger** manually cancels everything in the group and blocks new orders until reset (panic button).
- **Update limit**: if the new limit would immediately trigger, orders are cancelled and the group triggers.
- Deleting a group cancels all its orders.
- Real-time group lifecycle/limit events on the `order_group_updates` WS channel.

Recommended pattern: place all quoting orders with `order_group_id` set, size the 15s matched-contract limit to your adverse-selection tolerance, and pair with `cancel_order_on_pause=true`.

## 8. WebSocket API

### 8.1 Connection & keep-alive

- Single connection at the WS URL; **authentication is required to connect** (include the same three `KALSHI-ACCESS-*` headers on the handshake; method `GET`, path `/trade-api/ws/v2`), even for public channels.
- Kalshi sends WS **Ping (0x9)** every 10 s with body `heartbeat`; respond with **Pong (0xA)**. You may also send Pings; Kalshi responds Pong.

### 8.2 Command protocol

Client → server commands (JSON), each with a client-generated monotonically increasing `id` (0 = treated as absent):

```json
{"id": 1, "cmd": "subscribe", "params": {"channels": ["orderbook_delta"], "market_tickers": ["FED-23DEC-T3.00"]}}
{"id": 2, "cmd": "unsubscribe", "params": {"sids": [1, 2]}}
{"id": 3, "cmd": "list_subscriptions"}
{"id": 4, "cmd": "update_subscription", "params": {"sid": 456, "market_tickers": ["X"], "action": "add_markets"}}
```

- `subscribe` params: `channels` (required, array), plus market spec: `market_ticker` | `market_tickers` (mutually exclusive) or `market_id(s)` (ticker channel only). Options: `send_initial_snapshot` (ticker channel), `skip_ticker_ack`, `shard_factor`/`shard_key` (communications fanout).
- `update_subscription` actions: `add_markets`, `delete_markets`, `get_snapshot` (orderbook channel: returns fresh `orderbook_snapshot` without modifying the subscription — useful for resync).
- Server responses: `{"type":"subscribed","msg":{"channel":...,"sid":...}}`, `{"type":"ok",...}`, `{"type":"unsubscribed",...}`, or `{"type":"error","msg":{"code":N,"msg":"..."}}`.
- Every subscription gets a server-generated `sid`. Data messages carry `sid` and a per-subscription **`seq`**; **verify `seq` is gapless** — a gap means missed messages → resubscribe or `get_snapshot`.
- Error codes 1–22 (notables: 6 already subscribed, 7 unknown sid, 8 unknown channel, 9 auth required, 16 market not found, 18 command timeout).

### 8.3 Channels

| Channel | Auth | Market spec | Purpose |
|---|---|---|---|
| `orderbook_delta` | yes | required (tickers only) | Snapshot then incremental book deltas |
| `ticker` | conn-level | optional | Price/volume/OI updates; supports `send_initial_snapshot` |
| `trade` | conn-level | optional | Public trades (`taker_outcome_side`/`taker_book_side`) |
| `fill` | yes | optional (omit = all) | Your fills, pushed immediately |
| `user_orders` | yes | optional | Your order created/updated events |
| `market_positions` | yes | optional | Your position updates |
| `market_lifecycle_v2` | conn-level | — | Market state changes (non-MVE) + event creation |
| `multivariate_market_lifecycle` | conn-level | — | Same for `KXMVE*` markets |
| `order_group_updates` | yes | — | Group lifecycle/limit events |
| `communications` | yes | — | RFQ/quote notifications (sharding supported) |
| `multivariate` | — | — | Deprecated (pre-RFQ) |

### 8.4 `orderbook_delta` channel detail

On subscribe you receive `orderbook_snapshot`, then `orderbook_delta` messages:

```json
{"type":"orderbook_snapshot","sid":2,"seq":2,"msg":{
  "market_ticker":"FED-23DEC-T3.00","market_id":"...",
  "yes_dollars_fp":[["0.0800","300.00"],["0.2200","333.00"]],
  "no_dollars_fp":[["0.5400","20.00"],["0.5600","146.00"]]}}

{"type":"orderbook_delta","sid":2,"seq":3,"msg":{
  "market_ticker":"FED-23DEC-T3.00","market_id":"...",
  "price_dollars":"0.960","delta_fp":"-54.00","side":"yes","ts_ms":1669149841000}}
```

- Snapshot levels: `[price_dollars, count_fp]`; keys absent if a side is empty.
- Delta: `delta_fp` is a **signed increment** to the resting size at that `price_dollars`/`side` level (the live doc calls it a "fixed-point contract delta"; example values are signed, e.g. `-54.00`). Apply `size += delta_fp`; remove the level when its running size reaches ≤ 0. It is **not** an absolute replacement.
- Snapshot subscribe params (per the live channel doc): `market_ticker` (single) or `market_tickers` (array); `market_id`/`market_ids` are **not** supported for this channel. `update_subscription` supports `add_markets` / `delete_markets` / `get_snapshot`.
- `client_order_id` and `subaccount` appear on deltas **you** caused — use for low-latency ack/fill inference of your own quotes.
- `ts` (RFC3339) is deprecated; use `ts_ms`.
- **Pricing convention (NO side).** The book is bids-only on both sides (§4.4). In the examples above, NO-side levels are in **no-leg pricing** — a NO bid at `0.56` pairs with a YES ask at `1 − 0.56 = 0.44` — so YES and NO use different scales and `best_yes_ask = 1 − best_no_bid`. ⚠️ **`use_yes_price` is NOT documented on the current `orderbook_delta` channel** (an earlier revision described such a flag to report both sides on the yes scale; it no longer appears in the live docs). Do **not** rely on sending it. Treat NO-side levels as no-leg unless a live capture proves otherwise — and if you *do* send `use_yes_price: true` and the server honors it, your `1 − price` complement math would double-flip and invert the ask. **Verify the NO-side scale against a live demo capture before trusting the computed ask.**
- Sequence gaps: `seq` is a per-subscription counter that must be gapless; on a gap, resync via `update_subscription` `get_snapshot` or resubscribe.

### 8.5 `fill` channel message

```json
{"type":"fill","sid":13,"msg":{
  "trade_id":"...","order_id":"...","market_ticker":"HIGHNY-22DEC23-B53.5",
  "is_taker":true,"side":"yes","yes_price_dollars":"0.750","count_fp":"278.00",
  "fee_cost":"...","action":"buy","ts_ms":1671899397000,
  "client_order_id":"...","post_position_fp":"500.00","purchased_side":"yes","subaccount":3}}
```

Key fields: `trade_id` (unique per fill), `order_id`, `is_taker`, `yes_price_dollars` (always YES-leg price, `*_dollars` string), `count_fp` (fixed-point contracts, 2 dp), `post_position_fp` (position after fill), `ts_ms`. Direction: the schema lists `outcome_side` and `book_side` (current — bid≡yes, ask≡no); `action`/`side`/`purchased_side` are legacy (still shown in the doc's example payload). Prefer `outcome_side`/`book_side`.

- **`fee_cost`** (verified 2026-07-02): a fixed-point **dollars string** and the **TOTAL exchange fee for this fill** (not per-contract). Convert to cents with `×100`, subtract once per fill. Example: `"0.0175"` = 1.75¢ total for the fill.
- The illustrative example above omits `fee_cost`/`outcome_side`/`book_side` for brevity; they are defined in the channel schema. Fees can be `0` (maker fills on most markets are free — §10/§13).

## 9. Market Lifecycle, Pauses, Settlement

### Statuses (REST)
`initialized` → `active` → (`inactive` ⇄ `active`) → `closed` → `determined` (→ `disputed` → `amended`) → `finalized`.

Status filter mapping on `GET /markets?status=`: `unopened`→initialized, `open`→active, `paused`→inactive, `closed`→any post-close not finalized, `settled`→finalized.

### Transitions & WS events
- Implicit (NO WS event): `initialized→active` at `open_time`; `→closed` at `close_time`.
- Explicit (WS events on lifecycle channels): `deactivated`, `activated` (**all resting orders are cancelled when a paused market reactivates**), `close_date_updated` (close can be moved earlier if `can_close_early`), `determined`, `settled`.
- After `close_time`: **all order ops including cancels are rejected with `MARKET_INACTIVE`**; resting orders auto-cancel shortly after close (cancellation updates published on user channels).
- Markets may 404 on REST right after a `created` event — retry with backoff.
- Determination sets `result` ∈ {yes, no, scalar}; settlement timer (`settlement_timer_seconds`) runs before `finalized`; `settlement_ts` populated when done.

### Maintenance windows
- **Every Thursday 3:00–5:00 AM ET**: trading pause (rarely a full exchange pause). Expect disconnections; reconnect after 5:00 AM ET.
- Trading pause: no place/amend; **cancels allowed**; resting orders remain unless `cancel_order_on_pause`.
- Exchange pause: no place/amend/**cancel**; resting orders remain unless `cancel_order_on_pause`. An exchange pause outside the Thursday window = unscheduled outage.

## 10. Fees & Rounding

The `fee_rounding` doc covers the **rounding** mechanics (the base trade-fee formula lives on the fee schedule — see §13, verify against the live schedule). Per fill:

- **Target balance precision**: **$0.01 for non-direct members** (the common case), **$0.0001 for direct members**. Balances land on a multiple of that precision after every fill.
- **Rounding fee**: `balance_change` (= revenue − trade fee) is floored toward −∞ to the member's target balance precision; `rounding_fee = balance_change − floor(balance_change)` (the sub-precision remainder).
- **Rebate**: a per-order **fee accumulator** tracks cumulative rounding overpayment across all fills of an order (taker and maker alike); once the accumulated rounding exceeds $0.01, a whole-cent rebate is issued and the accumulator is reduced by $0.01. Total fee across many small fills converges to the single-fill equivalent.
- Net fee = trade fee + rounding fee − rebate ≥ $0.
- Settlement payouts are rounded **down**; the remainder is recorded as a settlement fee.

Model implication: track fees per-order, not per-fill in isolation; intermediates need 6-decimal precision when subpenny prices × fractional counts combine.

## 11. Pagination

Cursor-based on list endpoints (`/markets`, `/events`, `/series`, `/markets/trades`, `/portfolio/fills`, `/portfolio/orders`, etc.):
- `limit` (typically 1–100, default 100) and `cursor` params.
- Response includes `cursor`; pass it back as `?cursor=...`; done when absent/null.

## 12. FIX API (pointer)

Kalshi also offers FIX for order entry: sessions/logon with API-key auth, order entry (New Order Single 35=D; tag `21006` = CancelOrderOnPause), order groups, RFQ messages, drop copy (recover missed execution reports), listener sessions (read-only real-time exec reports), subpenny dollar-based pricing, and settlement reports. Docs under `https://docs.kalshi.com/fix/*`. Likely the right transport if you want lower-latency order entry than REST; market data still comes via WS.

## 13. Participation, Programs & Fee Economics

### Who can market-make
- No special status is required to quote two-sided markets via API. Any U.S. member (18+, full KYC, funded account) can run a market-making bot at the Basic rate tier. Kalshi states most of its market makers are individuals and small institutions.
- Constraints that apply to all members: per-market position limits (designated MMs receive adjusted limits), insider/source-agency prohibitions (no trading markets where you have MNPI or are a decision-maker on the outcome), and state-level restrictions on some contract categories (e.g. sports in certain states).

### Fee model (verify against Kalshi's live fee schedule; subject to change)
- Taker fee ≈ `roundup(0.07 × C × P × (1−P))` — maximized at P=$0.50 (~$1.75 per 100 contracts). Some series carry different multipliers.
- Maker (resting) orders are **fee-free on most markets**. A minority of flagged series carry a maker fee ≈ `roundup(0.0175 × C × P × (1−P))` (¼ of taker).
- Implication: passive quoting is nearly costless at entry on most markets; the fee model output feeds the trade-fee + rounding-fee + rebate mechanics in §10.

### Programs (escalating formality)
1. **Liquidity Incentive Program** — opt-in, open to most regular U.S. members (excludes Kalshi affiliates/employees, members with existing MM agreements, IB/FCM customers). Kalshi snapshots books every second during trading hours; resting orders are scored by size and proximity to best prices against a per-market target size (100–20,000 contracts) and discount factor; participants earn a pro-rata share of a per-market reward pool. Eligible markets/schedules are marked per market; incentives are also queryable via the incentives API endpoints.
2. **Rate-limit tiers** — Basic (default) → Advanced (application) → Premier/Paragon/Prime (relationship-based; Premier+ allows supplying your own API public key). See §3.
3. **Designated Market Maker program** — formal status under Rulebook Chapter 4. Selective: review of financial resources, trading experience, business reputation; requires defined quoting/volume obligations. Benefits can include fee discounts/rebates/revenue share, adjusted position limits, enhanced throughput, and risk tools such as cancel-on-disconnect order protection. Members with MM agreements become ineligible for the open Liquidity Incentive Program.

## 14. Market Maker Implementation Checklist

1. **Book building**: subscribe `orderbook_delta` (`market_tickers`); apply snapshot, then apply each `delta_fp` as a **signed increment** keyed by (ticker, side, price), removing a level at ≤ 0; enforce gapless `seq` per `sid`; on gap, `get_snapshot`. Best bid = highest price level; YES ask is implied as `1 − best NO bid`. Treat NO-side levels as **no-leg** pricing (see §8.4) — `use_yes_price` is no longer a documented flag; verify the NO scale against a live capture before trusting the ask.
2. **Own-order tracking**: `user_orders` + `fill` channels; correlate via `client_order_id` (also present on your own book deltas). Reconcile with REST (`user_data_timestamp` caveat: REST reads lag; trust write responses + WS).
3. **Quoting**: Create Order V2 with `post_only` for passive quotes, `time_in_force=good_till_canceled` (+ `expiration_time` for auto-expiry), `self_trade_prevention_type` chosen deliberately (`maker` keeps your taker flow alive; `taker_at_cross` protects resting quotes), `cancel_order_on_pause=true`, and `order_group_id` on everything.
4. **Risk**: one or more order groups with 15s rolling matched-contract limits; `trigger` as the kill switch; `reset` to resume. Respect the 200k open-order cap.
5. **Throughput budget**: cancels are 2 tokens vs 10 for creates — cancel/replace costs 12; amends cost 10. Batch calls don't reduce token cost, only HTTP overhead. Size quoting breadth to your tier's write budget; exploit the 2-second burst bucket for event-driven re-quoting.
6. **Numerics**: integer fixed-point internally (counts ×100, prices ×10,000); respect per-market `price_level_structure` ticks; handle fractional fills even if you only send whole contracts.
7. **Calendar**: pull `exchange/schedule`; flatten or rely on `cancel_order_on_pause` before Thursday 3–5 AM ET; subscribe `market_lifecycle_v2` and treat `deactivated`/`activated` (orders wiped on reactivation) and `close_date_updated` as first-class quote-pulling triggers.
8. **Backoff**: on 429, exponential backoff (no Retry-After header); on WS disconnect, reconnect, resubscribe, resnapshot.
9. **Economics**: opt into the Liquidity Incentive Program; bias quoting toward incentivized markets (query the incentives API for active reward periods, target sizes, discount factors) and toward series without maker fees. Score-per-snapshot rewards favor persistent size near the BBO over fleeting quotes.
