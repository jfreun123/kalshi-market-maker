# Kalshi API — Full Endpoint Catalog

> Compiled 2026-06-30 from a systematic sweep of every endpoint in the
> `docs.kalshi.com` API reference (the `llms.txt` index) plus the WebSocket
> channel docs. Companion to `kalshi-api.md` (auth/implementation notes). This
> file is the *catalog*: what exists, exact field names, and relevance to our
> market maker.

## Schema conventions & deprecation traps (read first)

The current docs use a **fixed-point decimal-string schema**. Getting field
names wrong here has already cost us two production bugs — treat this section as
load-bearing.

- **Prices:** strings suffixed `_dollars`, e.g. `yes_bid_dollars: "0.5200"` (up
  to 6 dp). Not integer cents.
- **Sizes / counts:** strings suffixed `_fp` (fixed-point, 2 dp), e.g.
  `count_fp: "5.52"`, `position_fp: "-4.36"`. **Fractional** — never round to
  int (that erases sub-unit orderbook deltas; see `types.hpp` `Quantity`).
- **Exceptions still in integer cents:** `balance`, `portfolio_value`,
  `revenue`, `amount_cents`, `price_centi_cents`, `period_reward` (centi-cents).
- **Deprecated fields that mis-signed us** — do **not** read these:
  | Deprecated | Use instead | Context |
  |---|---|---|
  | `side` (yes/no) on a fill | `purchased_side` (WS) / `outcome_side`+`book_side` (REST) | side we *acquired* vs aggressor/book side |
  | `action` (buy/sell) | `outcome_side` + `book_side` | order/fill direction |
  | `taker_side` on a trade | `taker_outcome_side` + `taker_book_side` | public tape aggressor |
  | `category` on events | series `tags`/`category` | — |
  | `liquidity_dollars` on markets | (nothing — always `"0.0000"`) | do not gate on it |
- **Position sign:** `position_fp` **positive = YES, negative = NO** (REST
  `/portfolio/positions`; WS `market_positions` almost certainly same — confirm).

## Confirmed against today's two bug fixes

1. **Orderbook delta** (`orderbook_delta.delta_fp`) is a **signed increment** to
   the running size at `(side, price_dollars)`; remove the level at size 0.
   Confirms commit `6d4ef7e`. Snapshot arrays are `yes_dollars_fp` /
   `no_dollars_fp` = `[[price_dollars, count_fp], …]`.
2. **Fill side**: sign inventory from **`purchased_side`** (the side our account
   acquired), not `side` (the book/aggressor side). Confirms today's fix. Even
   better: the WS fill carries **`post_position_fp`** — the exchange's
   authoritative net position after the fill. Prefer syncing to it over
   accumulating.

## Prioritized adopt-list for our market maker

| Priority | Endpoint / feature | Why |
|---|---|---|
| **P1** | `post_position_fp` on WS fill | Authoritative post-fill inventory — self-corrects the class of bug we just hit. Sync instead of accumulate. |
| **P1** | Order Groups (`order_group_id` + `/trigger` + `/reset`) | Native server-side circuit breaker: rolling-15s matched-contracts cap auto-cancels the group on breach. Stronger toxic-flow guard than our client-side `kModelDiverge`. |
| **P1** | `GET /incentive_programs` (`incentive_type=liquidity`) | Direct maker-rebate feed. `period_reward` (centi-cents) + `target_size_fp` → steer quoting to incentivized markets. |
| **P2** | `GET /portfolio/orders/queue_positions` (`queue_position_fp`) | The "P2 queue-awareness" PLAN item — direct adverse-selection / fill-probability signal, one call for the whole ladder. |
| **P2** | Batch order endpoints (`/orders/batched` create & cancel) | Refresh/pull the whole quote ladder in one request. Cost is per-item (10/create, 2/cancel) but one round-trip. |
| **P2** | `POST /markets/orderbooks/batch` | Batched book refresh across the quoting basket. |
| **P3** | `decrease` (not `amend`) for inventory trims | Amend only preserves queue priority when *decreasing* size; price change / size-up → back of queue. So amend-to-reprice ≠ free; use `decrease` for trims, cancel+replace for reprices. |
| **P3** | Fee feeds (`series_fee_changes`, `event_fee_changes`, `scheduled_ts`) | `quadratic_with_maker_fees` vs `flat` changes maker economics — pre-empt in pricing. |
| **P3** | `GET /exchange/status` gating | Gate all quoting on `trading_active`/`exchange_active`. |
| Defer | RFQ / block trades | Maker-in-size channel but request-driven, adverse-selection heavy, confirm-timer complexity — not worth it for a small bot yet. |

---

# REST endpoint reference

## Exchange
| Method | Path | Purpose / key fields |
|---|---|---|
| GET | `/exchange/status` | `exchange_active`, `trading_active`, `exchange_estimated_resume_time`, `exchange_index_statuses[]`. **Gate quoting on this.** |
| GET | `/exchange/schedule` | `standard_hours` (per-day open/close ET), `maintenance_windows[]` (`start_datetime`/`end_datetime`). |
| GET | `/exchange/announcements` | `announcements[]`: `type` (info/warning/error), `message`, `status`. |
| GET | `/series/fee_changes` | `series_fee_change_arr[]`: `fee_type` (quadratic/quadratic_with_maker_fees/flat), `fee_multiplier`, `scheduled_ts`. Params `series_ticker`, `show_historical`. |
| GET | `/exchange/user_data_timestamp` | `as_of_time` — freshness of user data (reconcile REST vs WS). |

## Markets / Series / Trades
| Method | Path | Purpose / key fields |
|---|---|---|
| GET | `/markets/{ticker}` | Full market. Prices `yes_bid_dollars`/`yes_ask_dollars`/`no_bid_dollars`/`no_ask_dollars`/`last_price_dollars`; sizes `yes_bid_size_fp`/`yes_ask_size_fp`; `volume_fp`/`volume_24h_fp`/`open_interest_fp`; `status`, `result`, `strike_type`, `floor_strike`/`cap_strike`, `price_ranges[]`, `fee_waiver_expiration_time`, `close_time`. |
| GET | `/markets` | List/scan. Params: `limit` (≤1000), `cursor`, `event_ticker`, `series_ticker`, `status` (unopened/open/paused/closed/settled), `tickers`, `min_updated_ts` (incremental poll), `mve_filter`. |
| GET | `/markets/{ticker}/orderbook` | `orderbook_fp` → `yes_dollars`/`no_dollars`, each `[[price_dollars, count_fp], …]`. **Bid side only** per side; `depth` param (0=all). |
| POST | `/markets/orderbooks/batch` | Up to 100 tickers → `orderbooks[]` (same `orderbook_fp` shape). **Adopt for basket refresh.** |
| GET | `/markets/{ticker}/candlesticks` | OHLC per market. `period_interval` ∈ {1,60,1440} min. `yes_bid`/`yes_ask` (`*_dollars` OHLC), `price` dist, `volume_fp`, `open_interest_fp`. |
| POST | `/markets/candlesticks/batch` | Up to 100 markets / 10k candles. |
| GET | `/series/{ticker}` | `fee_type`, `fee_multiplier`, `frequency`, `settlement_sources`, `category`, `tags`. |
| GET | `/series` | List series. `category`, `tags`, `min_updated_ts`. |
| GET | `/trades` | Public tape. `trades[]`: `trade_id`, `ticker`, `count_fp`, `yes_price_dollars`, `no_price_dollars`, **`taker_outcome_side`** (not `taker_side`), `taker_book_side`, `created_time`, `is_block_trade`. **Flow-imbalance signal.** |

## Events / Multivariate / Milestones / Structured Targets
| Method | Path | Purpose / key fields |
|---|---|---|
| GET | `/events/{ticker}` | `EventData`: `event_ticker`, `series_ticker`, **`mutually_exclusive`** (complement-pricing flag), `fee_type_override`/`fee_multiplier_override`, nested/separate `markets[]`. `with_nested_markets` param. |
| GET | `/events` | Scan (excludes multivariate). `limit` (≤200), `cursor`, `status`, `series_ticker`, `min_close_ts`, `min_updated_ts`, `with_nested_markets`, `with_milestones`. |
| GET | `/events/{ticker}/metadata` | Images, `settlement_sources`, `competition`. Cosmetic. |
| GET | `/events/{ticker}/candlesticks` | Aggregated OHLC across event's markets. |
| GET | `/events/fee_changes` | `event_fee_changes[]`: `fee_type_override`, `fee_multiplier_override`, `scheduled_ts`. |
| GET | `/events/{ticker}/forecast_percentile_history` | Percentile forecast points for scalar/range markets. |
| GET | `/events/multivariate` | Combo events from collections. |
| GET | `/multivariate_event_collections[/{ticker}]` | Collection rules: `size_min`/`size_max`, `is_all_yes`, `active_quoters`, `associated_events[]`. |
| POST | `/multivariate_event_collections/{ticker}/markets` | **Must instantiate a combo market before it can be quoted.** `selected_markets[]` (TickerPair: `market_ticker`,`event_ticker`,`side`). |
| GET | `/milestones[/{id}]` | Event timing (`start_date`/`end_date`, `related_event_tickers`). `GET /milestones` `limit` is **required** (1–500). |
| GET | `/structured_targets[/{id}]` | Entity dictionary (player/team). Uses `page_size` + `ids`. Low relevance. |

## Orders V2 (write path: `/portfolio/events/orders`)
**Write model:** `side` = **`bid`/`ask`** (not yes/no), single **`price`** (dollars
string), **`count`** (fp string), `time_in_force` (fill_or_kill /
good_till_canceled / immediate_or_cancel), `self_trade_prevention_type`
(taker_at_cross / maker), `post_only`, `reduce_only`, `client_order_id`,
`expiration_time` (unix **seconds**), `order_group_id`. No `type` field on write.
**Read model** returns `outcome_side`/`book_side` (current) + `side`/`action`
(deprecated), `yes_price_dollars`/`no_price_dollars`, `*_count_fp`, `status`
(resting/canceled/executed).

| Method | Path | Cost | Notes |
|---|---|---|---|
| POST | `/portfolio/events/orders` | 10 | Create. Returns `order_id`, `fill_count`, `remaining_count`, `average_fill_price`. |
| POST | `/portfolio/events/orders/batched` | 10/order | Batch create; per-order `error`. |
| DELETE | `/portfolio/events/orders/{id}` | 2 | Cancel; returns `reduced_by`. |
| DELETE | `/portfolio/events/orders/batched` | 2/order | Batch cancel (`orders[]` of objects). |
| POST | `/portfolio/events/orders/{id}/amend` | — | Change price/size in place. **Queue priority kept only when size DECREASES.** |
| POST | `/portfolio/events/orders/{id}/decrease` | — | `reduce_by` xor `reduce_to`. Size-down keeps queue priority. |
| GET | `/portfolio/orders/{id}` | 2 | Full order incl. `taker_fees_dollars`/`maker_fees_dollars`, `*_fill_cost_dollars`. |
| GET | `/portfolio/orders` | — | List (`status`, `ticker`, `event_ticker`≤10, `cursor`). Pre-cutoff → `/historical/orders`. |
| GET | `/portfolio/orders/{id}/queue_position` | — | `queue_position_fp` = shares ahead (price-time priority). |
| GET | `/portfolio/orders/queue_positions` | — | Bulk queue positions. **Adopt for adverse-selection scoring.** |

## Order Groups (native circuit breaker)
Rolling **15-second** matched-contracts cap; on breach **all group orders cancel
and no new orders until reset.**
| Method | Path | Notes |
|---|---|---|
| POST | `/portfolio/order_groups/create` | `contracts_limit`(_fp). Returns `order_group_id`. |
| GET | `/portfolio/order_groups[/{id}]` | `contracts_limit_fp`, `orders[]`, `is_auto_cancel_enabled`. |
| DELETE | `/portfolio/order_groups/{id}` | Delete + cancel all in group. |
| PUT | `/portfolio/order_groups/{id}/reset` | Re-arm after a breach. |
| PUT | `/portfolio/order_groups/{id}/trigger` | **Panic-cancel** the whole set, keep the group. |
| PUT | `/portfolio/order_groups/{id}/limit` | Change the cap live. |

## Portfolio
| Method | Path | Purpose / key fields |
|---|---|---|
| GET | `/portfolio/positions` | `market_positions[]`: `ticker`, `position_fp` (**+YES/−NO**), `market_exposure_dollars`, `realized_pnl_dollars`, `resting_orders_count`, `fees_paid_dollars`. `event_positions[]`. **Primary reconciliation source.** |
| GET | `/portfolio/fills` | `fills[]`: `fill_id`/`trade_id`, `order_id`, `ticker`, `outcome_side`, `book_side`, `count_fp`, `yes_price_dollars`, `no_price_dollars`, `is_taker`, `fee_cost`, `ts`. **No `purchased_side`, no `post_position` — REST differs from WS.** |
| GET | `/portfolio/balance` | `balance` (cents int), `balance_dollars`, `portfolio_value`, `balance_breakdown[]`. |
| GET | `/portfolio/settlements` | `market_result`, `revenue` (cents), `yes_count_fp`/`no_count_fp`, `settled_time`. |
| GET | `/portfolio/summary/total_resting_order_value` | `total_resting_order_value` (cents). FCM-oriented. |
| — | subaccounts / deposits / withdrawals / transfers / netting | Low relevance for our single-account bot. |

## Account / API Keys
| Method | Path | Notes |
|---|---|---|
| GET | `/account/limits` | `usage_tier`, `read`/`write` BucketLimit (`refill_rate`, `bucket_capacity`). **Pace order submission.** |
| GET | `/account/endpoint_costs` | `default_cost` (10) + per-endpoint overrides. |
| GET | `/account/api_usage_level/volume_progress` | Tier volume thresholds. |
| GET/POST/DELETE | `/api_keys[...]` | Key management. `generate` returns `private_key` once — **secret, never log/commit.** |

## Historical (backtest / archived)
Route by `GET /historical/cutoff` (`market_settled_ts`, `trades_created_ts`,
`orders_updated_ts`). All use `_dollars`/`_fp` schema.
| Method | Path | Notes |
|---|---|---|
| GET | `/historical/markets[/{ticker}]` | Archived markets (post-settlement). |
| GET | `/historical/markets/{ticker}/candlesticks` | OHLC + bid/ask distribution. **Backtest / fair-value calibration.** |
| GET | `/historical/fills` | Realized fills: `outcome_side`, `book_side`, `count_fp`, `is_taker`, `fee_cost`. Only `max_ts` filter. |
| GET | `/historical/orders` | Archived orders with maker/taker fee + fill-cost split. |
| GET | `/historical/trades` | Public tape: `taker_outcome_side`, `taker_book_side`. Both `min_ts`+`max_ts`. **Flow-imbalance backtest.** |

## Live Data / Search / Incentives / FCM / External
| Method | Path | Notes |
|---|---|---|
| GET | `/incentive_programs` | **Maker rebates.** `incentive_type` (liquidity/volume), `period_reward` (centi-cents), `target_size_fp`, `discount_factor_bps`, `market_ticker`, `start_date`/`end_date`, `status`. |
| GET | `/live_data/...` | Sports/game telemetry per milestone. Only for a sports fair-value model. |
| GET | `/search/filters_by_sport`, `/search/tags_by_categories` | Taxonomy only. |
| GET | `/fcm/orders`, `/fcm/positions` | FCM/subtrader-gated. Low relevance. |
| GET | `/cfbenchmarks/{path}` | Passthrough to CF Benchmarks index (e.g. BRTI Bitcoin index) for crypto fair-value. **Cost 50 tokens** — cache. |

## Communications / RFQ / Block Trades
Maker-in-size via RFQ: `GET /communications/rfqs` (discover) → `POST
/communications/quotes` (`yes_bid`/`no_bid` dollars) → creator `accept`s a side →
maker `confirm`s (starts execution timer) → `executed`. Block trades are
pre-negotiated vs a named `buyer_user_id`/`seller_user_id`. **Recommendation:
defer;** if pursued, start read-only on `GET /communications/rfqs`.

---

# WebSocket API

**Endpoint:** prod `wss://external-api-ws.kalshi.com/trade-api/ws/v2` (docs) /
`wss://api.elections.kalshi.com/trade-api/ws/v2` (our verified-live memory) —
**confirm host against working config before switching.** Demo
`wss://demo-api.kalshi.co/trade-api/ws/v2`.

**Auth:** same 3 headers as REST; sign `timestamp_ms + "GET" + "/trade-api/ws/v2"`
with RSA-PSS. **Subscribe:** `{"id":N,"cmd":"subscribe","params":{"channels":[…],
"market_tickers":[…]}}`. **Unsubscribe** by `sids`. **Keep-alive:** WS control
Ping/Pong (server pings ~10s with body `"heartbeat"`; reply Pong). Every data msg
carries monotonic **`seq`** per subscription — on a gap, drop the local book and
resubscribe for a fresh snapshot.

| Channel | type | Key fields |
|---|---|---|
| `orderbook_delta` (snapshot) | `orderbook_snapshot` | `yes_dollars_fp`/`no_dollars_fp` = `[[price_dollars, count_fp], …]`. |
| `orderbook_delta` (delta) | `orderbook_delta` | `price_dollars`, **`delta_fp` (SIGNED increment)**, `side` (yes/no). Apply `new = old + delta`; erase at 0. |
| `fill` | `fill` | `order_id`, `trade_id`, `market_ticker`, `is_taker`, `side` (book/aggressor — deprecated for sign), **`purchased_side`** (side we acquired), `action` (buy/sell), `yes_price_dollars`, `count_fp`, **`post_position_fp`** (authoritative net after fill), `ts_ms`. |
| `ticker` | `ticker` | `price_dollars`, `yes_bid_dollars`/`yes_ask_dollars`, `yes_bid_size_fp`/`yes_ask_size_fp`, `volume_fp`, `open_interest_fp`. **No no-side — derive as complement.** |
| `trade` | `trade` | `yes_price_dollars`, `no_price_dollars`, `count_fp`, `taker_outcome_side`, `taker_book_side`. |
| `user_orders` | `user_order` | `order_id`, `status`, `outcome_side`, `book_side`, `yes_price_dollars`, `fill_count_fp`/`remaining_count_fp`/`initial_count_fp`, fees. |
| `market_positions` | `market_position` | `position_fp` (+YES/−NO, confirm), `position_cost_dollars`, `realized_pnl_dollars`, `fees_paid_dollars`. |
| `market_lifecycle_v2` | `market_lifecycle_v2` | discriminated by `event_type` (created/activated/deactivated/determined/settled/…). Watch determined/settled to flatten. |
| `order_group_updates`, `communications`, `cfbenchmarks_value`, `multivariate*` | — | See `.md` pages if needed. |

## Cross-cutting: rate limits & pagination
- **Token bucket** per `/account/limits`: create=10, cancel=2, most reads=10,
  cfbenchmarks=50. Batch = per-item. 429 on exceed. Check `/account/endpoint_costs`.
- **Pagination:** `cursor` + `limit` (default 100, max 1000; events max 200;
  milestones 1–500 **required**; incentives max 10,000; structured targets use
  `page_size`/`ids`). Use `min_updated_ts`/`min_close_ts` for incremental polling.

## Source
Full per-endpoint digests captured 2026-06-30 via `docs.kalshi.com/*.md` +
`llms.txt`. WS field names cross-checked against a live `--capture` fill frame
(matched: `side`, `purchased_side`, `action`, `post_position_fp`, `is_taker`,
`count_fp`, `yes_price_dollars`).
