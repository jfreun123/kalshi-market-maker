# Kalshi API Reference

> Last verified against docs.kalshi.com on 2026-06-21.

## Environments

| Environment | REST base URL | WebSocket URL |
|---|---|---|
| Production | `https://external-api.kalshi.com/trade-api/v2` | `wss://external-api-ws.kalshi.com/trade-api/ws/v2` |
| Demo | `https://external-api.demo.kalshi.co/trade-api/v2` | `wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2` |

Credentials are not shared between environments. Demo API keys only work against demo endpoints.

## Authentication

Every request requires three headers:

```
KALSHI-ACCESS-KEY:       <api-key-id>
KALSHI-ACCESS-TIMESTAMP: <unix-milliseconds>
KALSHI-ACCESS-SIGNATURE: base64(RSA-PSS-SHA256(timestamp_ms + METHOD + path))
```

**Signature construction:**
```
message = str(timestamp_ms) + "GET" + "/trade-api/v2/markets"
          ↑ no separators, concatenated directly; path has NO query params
```

Algorithm: RSA-PSS, SHA-256, MGF1 with SHA-256, salt length = digest length.

Keys are generated at kalshi.com → Settings → API Keys. The private key is downloaded once — Kalshi does not retain it. See `source/auth.hpp` for the C++ implementation.

## Price and Count Representation (current API)

All prices use `_dollars` suffix: fixed-point dollar strings.
- `"0.5200"` = 52 cents = 52% probability
- Range: `"0.0100"` – `"0.9900"` for standard markets

All quantities use `_fp` suffix: fixed-point strings, 2 decimal places.
- `"10.00"` = 10 contracts

**Tick size** depends on `price_level_structure` field on the market:
- `linear_cent` — $0.01 tick (most event contracts)
- `tapered_deci_cent` — $0.001 at extremes (<$0.10, >$0.90), $0.01 in middle
- `deci_cent` — $0.001 everywhere (financial markets)

Our `price_cents` int model assumes `linear_cent`. Sub-cent markets need care.

## Order Direction

Two equivalent vocabularies — use either, not both:

| Vocabulary | Long Yes | Long No |
|---|---|---|
| `outcome_side` | `yes` | `no` |
| `book_side` | `bid` | `ask` |

`bid` = `yes`. `ask` = `no`. There is no true "No contract" — a No order is routed
as the complement of a Yes order. Legacy `side`/`action` fields deprecated May 28 2026.

## Key Endpoints

### Markets

```
GET /markets                         # list markets (filter: ?event_ticker=...)
GET /markets/{ticker}                # single market
GET /markets/{ticker}/orderbook      # REST orderbook snapshot
```

**Market response fields (key subset):**

| Field | Type | Notes |
|---|---|---|
| `ticker` | string | Unique market ID |
| `close_time` | ISO8601 | When trading stops; all orders rejected after this |
| `status` | enum | `initialized`\|`inactive`\|`active`\|`closed`\|`determined`\|`disputed`\|`amended`\|`finalized` |
| `yes_bid_dollars` | string | Best YES bid in dollars |
| `yes_ask_dollars` | string | Best YES ask in dollars |
| `last_price_dollars` | string | Last YES trade price |
| `result` | enum | `"yes"`\|`"no"`\|`"scalar"`\|`""` (empty until determined) |
| `price_level_structure` | string | Tick size regime |

### Orders

```
POST   /portfolio/events/orders      # place order (V2)
GET    /portfolio/orders?status=resting
DELETE /portfolio/orders/{order_id}  # cancel
```

**Create order request body (V2):**

| Field | Required | Notes |
|---|---|---|
| `ticker` | yes | Market identifier |
| `side` | yes | `bid` (long yes) or `ask` (long no) |
| `count` | yes | Fixed-point string e.g. `"10.00"` |
| `price` | yes | Fixed-point dollars e.g. `"0.5200"` — always in YES dimension |
| `time_in_force` | yes | `good_till_canceled` \| `fill_or_kill` \| `immediate_or_cancel` |
| `self_trade_prevention_type` | yes | `taker_at_cross` \| `maker` |
| `post_only` | no | Set `true` for passive MM quotes (rejects if would cross) |
| `cancel_order_on_pause` | no | Set `true` to auto-cancel when exchange pauses market |

**Price field for No orders:** always pass YES price = `100 - no_price_cents`. The API
uses a single YES-dimension price book. See `rest_client.cpp::place_order`.

**Create order response:**

```json
{
  "order_id": "3b23c1c7-...",
  "fill_count": "0.00",
  "remaining_count": "10.00",
  "ts_ms": 1715793600123
}
```

### Portfolio

```
GET /portfolio/balance
GET /portfolio/positions
GET /portfolio/fills
```

## Orderbook Format (REST)

`GET /markets/{ticker}/orderbook` returns:

```json
{
  "orderbook_fp": {
    "yes_dollars": [["0.5100", "150.00"], ["0.5200", "200.00"]],
    "no_dollars":  [["0.4700", "100.00"], ["0.4800", "150.00"]]
  }
}
```

Each entry is `[price_dollars, count_fp]`. Arrays sorted **ascending** by price
(best bid = **last** element). The YES ask is implied: `1.00 - best_no_bid`.

**Spread derivation:**
```
best_yes_bid = yes_dollars[-1][0]        e.g. "0.5200" = 52¢
best_yes_ask = 1.00 - no_dollars[-1][0]  e.g. 1.00 - 0.48 = 0.52 → 52¢
mid          = (best_yes_bid + best_yes_ask) / 2.0
```

## WebSocket Feed

```
wss://external-api-ws.kalshi.com/trade-api/ws/v2
```

Authentication: include `KALSHI-ACCESS-KEY`, `KALSHI-ACCESS-TIMESTAMP`,
`KALSHI-ACCESS-SIGNATURE` as HTTP headers during the WebSocket handshake.

**Subscribe command format:**
```json
{ "id": 1, "cmd": "subscribe", "params": { "channels": ["orderbook_delta"], "market_tickers": ["KXBTCD-25DEC31"] } }
```

For user-level channels (`fill`, `user_orders`) omit `market_tickers`:
```json
{ "id": 2, "cmd": "subscribe", "params": { "channels": ["fill"] } }
```

### orderbook_snapshot

Sent immediately on subscription:

```json
{
  "type": "orderbook_snapshot",
  "sid": 1,
  "seq": 1,
  "msg": {
    "market_ticker": "KXBTCD-25DEC31",
    "yes_dollars_fp": [["0.5100", "150.00"], ["0.5200", "200.00"]],
    "no_dollars_fp":  [["0.4700", "100.00"], ["0.4800", "150.00"]]
  }
}
```

### orderbook_delta

Incremental update after snapshot:

```json
{
  "type": "orderbook_delta",
  "sid": 1,
  "seq": 2,
  "msg": {
    "market_ticker": "KXBTCD-25DEC31",
    "side": "yes",
    "price_dollars": "0.5200",
    "delta_fp": "-5.00",
    "ts_ms": 1715793600123
  }
}
```

Apply rule: `level[side][price] += delta_fp`. Remove level when result ≤ 0.

### fill

Fired when one of your resting orders is filled:

```json
{
  "type": "fill",
  "sid": 2,
  "msg": {
    "trade_id": "...",
    "order_id": "...",
    "market_ticker": "KXBTCD-25DEC31",
    "is_taker": false,
    "side": "yes",
    "yes_price_dollars": "0.5200",
    "count_fp": "10.00",
    "fee_cost": "0.0100",
    "action": "buy",
    "ts_ms": 1715793600123,
    "post_position_fp": "10.00",
    "outcome_side": "yes",
    "book_side": "bid"
  }
}
```

`is_taker: false` = we were the passive (maker) side — expected for LP fills.

## Market Lifecycle

```
initialized → active (at open_time)
active ↔ inactive (exchange pause/unpause)
active/inactive → closed (at close_time) — all orders auto-cancelled
closed → determined → finalized
```

Subscribe to `market_lifecycle_v2` channel to track state transitions.
After `close_time`, all order operations return `MARKET_INACTIVE`.

## Rate Limits

Token bucket — separate Read and Write buckets, 10 tokens per request (default).

| Tier | Read tok/s | Write tok/s | Earn threshold (30d vol share) |
|---|---|---|---|
| Basic | 200 | 100 | — |
| Advanced | 300 | 300 | — |
| Expert | 600 | 600 | 0.15% |
| Premier | 1,000 | 1,000 | 0.25% |
| Paragon | 2,000 | 2,000 | 0.50% |

Basic = 10 orders/second sustained. Batch orders cost 10×N tokens (no discount).
429 response on exhaustion; no penalty, bucket refills continuously.

## Fees

Kalshi charges fees on fills. No settlement fee for binary YES/NO markets.
The `fee_cost` field on fill events gives the per-fill fee in dollars.

Fee reduces net PnL; factor it into minimum required spread:
```
min_spread_for_breakeven ≥ 2 × fee_per_contract
```
