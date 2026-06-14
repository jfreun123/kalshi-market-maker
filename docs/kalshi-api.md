# Kalshi API Reference

## Environments

| Environment | Base URL | Web UI |
|---|---|---|
| Production | `https://trading-api.kalshi.com/trade-api/v2` | kalshi.com |
| Demo | `https://demo-api.kalshi.co/trade-api/v2` | demo.kalshi.co |

The demo environment is a full paper-trading sandbox with the same API contract as production. Create a separate account at demo.kalshi.co. API keys are environment-specific — a prod key will not work on demo.

## Authentication

Every request requires three headers signed with your RSA-2048 private key:

```
Kalshi-Access-Key:       <api-key-id>
Kalshi-Access-Timestamp: <unix-milliseconds>
Kalshi-Access-Signature: <base64(RSA_SHA256(timestamp_ms + METHOD + path))>
```

**Message construction:**
```
message = str(timestamp_ms) + "GET" + "/trade-api/v2/markets"
          ↑ no separators, concatenated directly
```

Keys are generated at kalshi.com/account/api. The private key is downloaded once at creation time — store it securely, Kalshi does not retain it.

See `source/auth.hpp` for the implementation.

## Key Endpoints

### Markets

```
GET /markets
GET /markets?event_ticker=KXBTCD    # filter by event
GET /markets/{ticker}
GET /markets/{ticker}/orderbook
```

### Orders

```
POST   /orders                  # place a new order
GET    /orders?status=resting   # list orders
GET    /orders/{order_id}
DELETE /orders/{order_id}       # cancel
```

**Place order body:**
```json
{
  "ticker": "KXBTCD-25DEC31-T50000",
  "side": "yes",
  "action": "buy",
  "type": "limit",
  "count": 10,
  "yes_price": 52
}
```

`yes_price` is in cents (1–99). Kalshi has no separate `no_price` field — when buying NO, pass `yes_price = 100 - your_no_price`.

### Fills

```
GET /fills?ticker=KXBTCD-25DEC31-T50000
```

### Portfolio

```
GET /portfolio/balance
GET /portfolio/positions
```

## Orderbook Format

```json
{
  "orderbook": {
    "yes": [[52, 200], [51, 150]],
    "no":  [[45, 100], [44, 300]]
  }
}
```

Each entry is `[price_cents, quantity]`. YES levels are sorted descending by price. The implied YES ask = `100 - best_no_bid`. There is no crossed market by definition.

**Best bid/ask derivation:**
```
best_yes_bid = yes[0][0]          (highest price someone will buy YES)
best_yes_ask = 100 - no[0][0]     (cheapest YES implied from NO bids)
mid          = (best_yes_bid + best_yes_ask) / 2.0
```

## WebSocket Feed

```
wss://trading-api.kalshi.com/trade-api/ws/v2
```

Authentication uses the same RSA-SHA256 scheme, passed as a `login` message after connecting.

**Subscribe to an orderbook:**
```json
{ "id": 1, "cmd": "subscribe", "params": { "channels": ["orderbook_delta"], "market_tickers": ["KXBTCD-25DEC31-T50000"] } }
```

**Message types received:**

| Type | Payload |
|---|---|
| `orderbook_snapshot` | Full orderbook state on subscribe |
| `orderbook_delta` | `{ side, price, delta }` — `delta < 0` means removal |
| `fill` | Your order was filled |
| `order` | Order status change |

**Delta application rule:**
```
new_quantity = old_quantity + delta
if new_quantity <= 0: remove level
```

## Rate Limits

- REST: ~10 requests/second per key (verify in Kalshi docs — subject to change)
- WebSocket: one connection per key recommended; reconnect with exponential backoff

## Fees

Kalshi charges a fee on fills, expressed in basis points of the notional value. The fee is embedded in `Market.fee_rate_bps`. It reduces net PnL but does not affect quote pricing directly — factor it into your minimum required spread.

```
fee_per_contract = price_cents / 100.0 * fee_rate_bps / 10000.0
min_spread_for_breakeven = 2 * fee_per_contract * 100  (in cents)
```
