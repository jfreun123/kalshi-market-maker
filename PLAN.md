# Kalshi Market Maker — Build Plan

## Overall Architecture

```mermaid
graph TD
    subgraph Exchange ["Kalshi Exchange"]
        REST["REST API"]
        WS["WebSocket Feed"]
    end

    subgraph Core ["Market Maker Core"]
        AUTH["Auth Layer\n(RSA-SHA256)"]
        HTTP["REST Client"]
        WSCONN["WebSocket Client"]
        OB["Local Orderbook"]
        OM["Order Manager"]
        RM["Risk Manager"]
        FV["Fair Value Engine"]
        QE["Quoter"]
        LOOP["Main Loop"]
    end

    REST <-->|signed requests| AUTH
    AUTH --> HTTP
    WS -->|orderbook / fills| WSCONN
    WSCONN --> OB
    HTTP --> OM
    OB --> FV
    OB --> QE
    FV --> QE
    RM --> QE
    QE --> OM
    OM --> HTTP
    OM --> RM
    LOOP --> QE
    LOOP --> RM
```

---

## UAT Blockers

These must be resolved before any live or demo-exchange testing.

### BLOCKER-1: IxWebSocket production implementation is a stub

**File:** `source/websocket_client.cpp`, lines 22–50

`IxWebSocket` — the concrete class used in production — throws
`std::runtime_error("IxWebSocket: not yet implemented")` on every method
(`connect`, `send`, `on_message`, `on_connect`, `on_disconnect`, `run`,
`stop`). The `IWebSocket` interface and all unit tests use `FakeWebSocket`
(synchronous test double), so every test passes, but the binary cannot make a
real WebSocket connection.

Additionally, the `IXWebSocket` C++ library (`machinezone/IXWebSocket`) is not
fetched by CMake — it appears in no `cmake/*.cmake` file.

**Fix:**
1. Add `cmake/ixwebsocket.cmake` that fetches `machinezone/IXWebSocket` via
   FetchContent and links it to `kalshi_lib`.
2. Implement each `IxWebSocket` method by delegating to the underlying
   `ix::WebSocket` object stored in `IxWebSocket::Impl`.

---

### BLOCKER-2: REST API paths and field names unverified against UAT

**File:** `source/rest_client.cpp`

The REST client was written against documented API shapes but has not been
tested against the live UAT environment (`https://demo-api.kalshi.co/trade-api/v2`).
Kalshi has changed field names across API versions. A single wrong field name
silently breaks order placement or parsing without a compile-time error.

Fields most likely to drift: `yes_price`/`no_price` vs `price`, `count` vs
`quantity`, `status` string values (`resting` vs `open`), timestamp format.

**Fix:** Run against UAT with a single ticker, log raw request/response bodies,
and reconcile every field against Kalshi's current API reference before trading.

---

### Pre-UAT Checklist

- [ ] BLOCKER-1: Implement `IxWebSocket` using the real `ix::WebSocket` library
- [ ] BLOCKER-1: Add `cmake/ixwebsocket.cmake` FetchContent fetch
- [ ] BLOCKER-2: Raw REST request/response bodies verified against UAT
- [ ] Demo account created at kalshi.com, API key + RSA key pair generated
- [ ] `config.json` created from `config.example.json` with demo base URL
- [ ] Paper mode (`--paper`) runs to completion without errors
- [ ] Phase 14 (structured logging) in place so UAT output is readable

---

## Phase 1 — Types & Domain Model

Define the shared data structures the entire system depends on. No I/O, no logic — just plain structs and enums. This is the foundation everything else builds on.

**TDD approach:** Write tests that construct, copy, compare, and serialize each type. Implementation is trivial; the point is locking the interface.

```mermaid
graph TD
    T["types.hpp\nMarket, Order, Fill\nOrderbook, Side, Status"]

    style T fill:#555,color:#fff
```

**Files:**
- `source/types.hpp`
- `test/source/types_test.cpp`

**Testing:**
- **Unit:** struct construction, field access, equality, `remaining_quantity()`, `is_active()`, `is_valid_price()`, `complement_price()` symmetry. All done — 19 tests passing.
- **Contract/Integration/ASAN:** N/A — pure value types, no I/O.

**Key types:**

```cpp
enum class Side { Yes, No };
enum class OrderStatus { Open, PartiallyFilled, Filled, Cancelled };
enum class OrderType { Limit, Market };

struct Order {
    std::string id;
    std::string market_ticker;
    Side side;
    int price_cents;      // 1–99
    int quantity;         // number of contracts
    int filled_quantity;
    OrderStatus status;
    std::chrono::system_clock::time_point created_at;
};

struct Level {
    int price_cents;
    int quantity;
};

struct Orderbook {
    std::string market_ticker;
    std::vector<Level> yes;  // sorted descending
    std::vector<Level> no;   // sorted descending
};

struct Fill {
    std::string order_id;
    std::string market_ticker;
    Side side;
    int price_cents;
    int quantity;
    std::chrono::system_clock::time_point timestamp;
};

struct Market {
    std::string ticker;
    std::string title;
    int min_tick;       // always 1
    int fee_rate_bps;
    std::chrono::system_clock::time_point close_time;
};
```

---

## Phase 2 — Authentication

Kalshi v2 requires every REST request to be signed with an RSA-SHA256 private key. This is the critical first integration point.

**TDD approach:** Sign a known message with a known test key, verify the base64 output matches the expected value. All tests are purely computational — no network.

```mermaid
graph TD
    T["types.hpp"]
    AUTH["auth.hpp\nRSA-SHA256 signer\nHeader builder"]

    T --> AUTH

    style T fill:#555,color:#fff
    style AUTH fill:#555,color:#fff
```

**Files:**
- `source/auth.hpp` / `source/auth.cpp`
- `test/source/auth_test.cpp`

**Interface:**

```cpp
class Auth {
public:
    explicit Auth(std::string api_key, std::string pem_private_key);

    // Returns headers to attach to every request
    std::map<std::string, std::string> sign(
        std::string_view method,   // "GET", "POST", etc.
        std::string_view path      // "/trade-api/v2/markets"
    ) const;
};
```

**Headers produced:**
```
Kalshi-Access-Key: <api_key>
Kalshi-Access-Timestamp: <unix_ms>
Kalshi-Access-Signature: <base64(RSA_SHA256(timestamp + method + path))>
```

**Dependency:** OpenSSL (`find_package(OpenSSL REQUIRED)` in CMake).

**Testing:**
- **Unit:** generate an RSA keypair at test time, sign a message, verify the signature with the public key. Test header names, API key passthrough, timestamp bounds, invalid key throws. All done — 7 tests passing.
- **Integration** (`KALSHI_INTEGRATION_TESTS=ON`): sign a real `GET /markets` request against the demo API and assert HTTP 200. This proves the key format, timestamp tolerance, and base64 encoding are all correct end-to-end.
- **ASAN:** run `cmake --preset=asan` — OpenSSL memory handling is a common source of leaks under sanitizers.

---

## Phase 3 — REST Client

Thin HTTP wrapper around libcurl. Responsible only for making signed requests and returning raw JSON strings. JSON parsing happens in callers.

**TDD approach:** Inject an `IHttpTransport` interface. Unit tests use a `FakeTransport` that returns canned responses. A separate integration test (gated by `KALSHI_INTEGRATION_TESTS=ON`) hits the real API.

```mermaid
graph TD
    T["types.hpp"]
    AUTH["auth.hpp"]
    IHTTP["IHttpTransport\n(interface)"]
    CURL["HttpTransport\n(cpp-httplib impl)"]
    FAKE["FakeTransport\n(test double)"]
    RC["RestClient\nGET /markets\nGET /orderbook\nPOST /orders\nDELETE /orders"]

    T --> RC
    AUTH --> RC
    IHTTP --> RC
    IHTTP --> CURL
    IHTTP --> FAKE

    style T fill:#555,color:#fff
    style AUTH fill:#555,color:#fff
    style RC fill:#2a6,color:#fff
    style IHTTP fill:#2a6,color:#fff
    style CURL fill:#2a6,color:#fff
    style FAKE fill:#2a6,color:#fff
```

**Files:**
- `source/http_transport.hpp` (interface + CurlTransport)
- `source/rest_client.hpp` / `source/rest_client.cpp`
- `test/source/rest_client_test.cpp`

**Testing:**
- **Unit:** inject `FakeTransport` returning canned JSON strings. Test each method (`get_markets`, `get_orderbook`, `place_order`, `cancel_order`) parses the response into the correct domain struct. Test HTTP error codes (4xx, 5xx) throw or return expected errors.
- **Contract:** record real responses from the demo API into `test/fixtures/` (e.g. `orderbook_KXBTCD.json`). Add a test that parses the fixture — catches API schema drift without needing a live connection.
- **Integration** (`KALSHI_INTEGRATION_TESTS=ON`): call `get_markets()` and `get_orderbook(ticker)` against demo. Assert the returned structs are well-formed (non-empty ticker, valid price levels).
- **ASAN:** libcurl manages its own memory; run under ASAN to confirm no leaks in our wrapper.

**Interface:**

```cpp
class RestClient {
public:
    RestClient(const Auth& auth, std::unique_ptr<IHttpTransport> transport,
               std::string base_url = "https://trading-api.kalshi.com/trade-api/v2");

    std::vector<Market> get_markets(std::string_view event_ticker = "");
    Orderbook get_orderbook(std::string_view ticker);
    Order place_order(std::string_view ticker, Side side,
                      int price_cents, int quantity, OrderType type);
    bool cancel_order(std::string_view order_id);
    std::vector<Order> get_open_orders();
};
```

**Sequence (place order):**

```mermaid
sequenceDiagram
    participant Q as Quoter
    participant RC as RestClient
    participant A as Auth
    participant T as CurlTransport
    participant K as Kalshi

    Q->>RC: place_order(ticker, Yes, 52, 10)
    RC->>A: sign("POST", "/trade-api/v2/orders")
    A-->>RC: signed headers
    RC->>T: POST /orders + headers + body
    T->>K: HTTP request
    K-->>T: 201 JSON response
    T-->>RC: response body
    RC-->>Q: Order struct
```

---

## Phase 4 — Local Orderbook

Maintains an in-memory mirror of the exchange orderbook, updated via WebSocket deltas. Provides fast BBO (best bid/offer) lookups without network round trips.

**TDD approach:** Feed sequences of delta messages into the orderbook and assert the resulting state. Test BBO calculation, mid-price, and edge cases (empty book, locked market).

```mermaid
graph TD
    T["types.hpp"]
    AUTH["auth.hpp"]
    RC["RestClient"]
    OB["LocalOrderbook\nSnapshot init\nDelta apply\nBBO / mid\nSpread query"]

    T --> OB
    RC -->|snapshot on connect| OB

    style T fill:#555,color:#fff
    style AUTH fill:#555,color:#fff
    style RC fill:#555,color:#fff
    style OB fill:#2a6,color:#fff
```

**Files:**
- `source/orderbook.hpp` / `source/orderbook.cpp`
- `test/source/orderbook_test.cpp`

**Interface:**

```cpp
class LocalOrderbook {
public:
    void apply_snapshot(const Orderbook& snap);
    void apply_delta(Side side, int price_cents, int new_quantity); // quantity=0 means remove

    std::optional<Level> best_bid() const;  // highest yes price
    std::optional<Level> best_ask() const;  // lowest yes ask (= 100 - best no bid)
    double mid_price_cents() const;
    int spread_cents() const;

    const Orderbook& state() const;
};
```

**Note on Kalshi orderbook mechanics:** YES and NO are two sides of the same contract. The YES ask price = `100 - NO bid price`. Kalshi's feed gives you both YES and NO levels; the implied best ask on YES comes from the best bid on NO.

**Testing:**
- **Unit:** apply a known sequence of snapshot + deltas, assert exact BBO and mid after each step. Test edge cases: empty book returns `std::nullopt` for BBO, single-sided book, delta that removes a level (quantity = 0), delta that adds a new level at a new price.
- **Contract:** capture a real WebSocket `orderbook_snapshot` message from the demo API, save to `test/fixtures/orderbook_snapshot.json`, write a test that applies it and checks BBO is in [1,99].
- **Integration/ASAN:** N/A — pure in-memory logic; covered by unit tests under ASAN.

---

## Phase 5 — WebSocket Client

Streams real-time orderbook snapshots and delta updates, and fill confirmations. Drives the `LocalOrderbook` and `OrderManager` with live data.

**TDD approach:** Mock the WebSocket connection with a `FakeWebSocket` that replays recorded message sequences. Test that the client correctly dispatches snapshot vs. delta messages and calls the registered callbacks.

```mermaid
graph TD
    T["types.hpp"]
    RC["RestClient"]
    OB["LocalOrderbook"]
    IWS["IWebSocket\n(interface)"]
    BWSS["IxWebSocket\n(stub, impl Phase 10)"]
    FWS["FakeWebSocket\n(test double)"]
    WSC["WebSocketClient\nSubscribe markets\nDispatch callbacks\nReconnect logic"]

    T --> WSC
    IWS --> WSC
    IWS --> BWSS
    IWS --> FWS
    WSC -->|orderbook updates| OB

    style T fill:#555,color:#fff
    style RC fill:#555,color:#fff
    style OB fill:#555,color:#fff
    style WSC fill:#555,color:#fff
    style IWS fill:#555,color:#fff
    style BWSS fill:#555,color:#fff
    style FWS fill:#555,color:#fff
```

**Files:**
- `source/websocket_client.hpp` / `source/websocket_client.cpp`
- `test/source/websocket_client_test.cpp`

**Testing:**
- **Unit:** inject `FakeWebSocket` that replays a scripted sequence of raw JSON messages. Assert `on_orderbook_snapshot`, `on_orderbook_delta`, and `on_fill` callbacks fire with correctly parsed payloads. Test reconnection: simulate a disconnect, assert the client attempts to reconnect and re-subscribes.
- **Contract:** capture a real `orderbook_delta` and `fill` message from the demo API into `test/fixtures/`. Test parsing in isolation.
- **Integration** (`KALSHI_INTEGRATION_TESTS=ON`): connect to the demo WebSocket, subscribe to one market, receive at least one delta within a timeout. Verifies auth handshake, subscription format, and message parsing end-to-end.
- **TSAN:** the WebSocket client runs a background thread; run under ThreadSanitizer to catch races between the callback thread and the main loop reading `LocalOrderbook`.

**Callbacks registered by the main loop:**

```cpp
ws_client.on_orderbook_snapshot([&](const Orderbook& snap) {
    local_ob.apply_snapshot(snap);
});
ws_client.on_orderbook_delta([&](const std::string& ticker, Side side,
                                  int price, int qty) {
    local_ob.apply_delta(side, price, qty);
});
ws_client.on_fill([&](const Fill& fill) {
    order_manager.record_fill(fill);
});
```

---

## Phase 6 — Order Manager

Tracks the lifecycle of all live orders. Provides a clean interface for placing, cancelling, and amending quotes. Reconciles local state with fill confirmations.

**TDD approach:** Drive the order manager through state transitions with synthetic fills and cancellations. Test that duplicate fills are idempotent, that cancelling a filled order is handled gracefully, and that the position calculation is correct.

```mermaid
graph TD
    T["types.hpp"]
    RC["RestClient"]
    OB["LocalOrderbook"]
    WSC["WebSocketClient"]
    OM["OrderManager\nPlace / cancel orders\nTrack open orders\nAccumulate fills\nNet position per market"]

    T --> OM
    RC --> OM
    WSC -->|fill events| OM

    style T fill:#555,color:#fff
    style RC fill:#555,color:#fff
    style OB fill:#555,color:#fff
    style WSC fill:#555,color:#fff
    style OM fill:#555,color:#fff
```

**Files:**
- `source/order_manager.hpp` / `source/order_manager.cpp`
- `test/source/order_manager_test.cpp`

**Interface:**

```cpp
class OrderManager {
public:
    Order place(const std::string& ticker, Side side,
                int price_cents, int quantity);
    bool cancel(const std::string& order_id);
    void cancel_all(const std::string& ticker);

    void record_fill(const Fill& fill);

    // Returns net YES position (negative = net NO)
    int net_position(const std::string& ticker) const;
    double realized_pnl(const std::string& ticker) const;

    const std::unordered_map<std::string, Order>& open_orders() const;
};
```

**Testing:**
- **Unit:** drive the state machine with synthetic fills and cancellations. Test that a duplicate fill is idempotent. Test that cancelling a filled order is handled gracefully. Test `net_position()` across a sequence of YES buys and NO buys (which reduce YES position). Test `realized_pnl()` against a known sequence of fills.
- **Contract:** record a real order JSON response from the demo API into `test/fixtures/order_placed.json`. Test that `place_order()` parses it correctly.
- **Integration** (`KALSHI_INTEGRATION_TESTS=ON`): place a 1-contract limit order at an extreme price (1 cent) on the demo API — unlikely to fill. Assert order appears in `get_open_orders()`. Cancel it. Assert it no longer appears.
- **ASAN:** `unordered_map` iteration during cancellation is a common bug site.

---

## Phase 7 — Risk Manager

Guards all outgoing order actions. Acts as a gatekeeper that the Quoter must pass through before any order reaches the exchange. Think of this as your pre-trade risk checks.

**TDD approach:** Write tests that try to breach each limit and confirm the order is rejected. Test PnL accumulation over a sequence of fills. Test that the daily loss limit triggers a full cancel-and-halt.

```mermaid
graph TD
    T["types.hpp"]
    OM["OrderManager"]
    RM["RiskManager\nMax position per market\nMax open orders\nDaily loss limit\nOrder size limit\nKill switch"]

    T --> RM
    OM -->|position / PnL feed| RM
    RM -->|approve / reject| QE["Quoter (Phase 8)"]

    style T fill:#555,color:#fff
    style OM fill:#555,color:#fff
    style RM fill:#555,color:#fff
    style QE fill:#aaa,color:#fff
```

**Files:**
- `source/risk_manager.hpp` / `source/risk_manager.cpp`
- `test/source/risk_manager_test.cpp`

**Interface:**

```cpp
struct RiskLimits {
    int max_position_per_market = 100;   // contracts
    int max_open_orders_per_market = 4;
    int max_order_size = 25;             // contracts
    double daily_loss_limit = -500.0;    // dollars
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits);

    bool check_order(const std::string& ticker, Side side,
                     int price_cents, int quantity) const;

    void update(const OrderManager& om);

    bool is_halted() const;
    void halt();   // manual kill switch
    void resume();
};
```

**Testing:**
- **Unit:** attempt to breach each limit individually and assert `check_order()` returns false. Test that breaching the daily loss limit triggers `is_halted()`. Test `halt()` blocks all subsequent orders. Test `resume()` restores normal operation. Test that limits are evaluated independently (e.g. position OK but order size too large → reject).
- **Integration/Contract/ASAN:** N/A — pure logic, no I/O. Run unit tests under ASAN.

---

## Phase 8 — Fair Value Engine

Estimates the true probability of the event. This is where alpha lives. Start simple and layer complexity.

**TDD approach:** Each model is a pure function from inputs to a probability in [0,1]. Tests are deterministic — feed known inputs, assert expected outputs. No randomness, no I/O.

```mermaid
graph TD
    T["types.hpp"]
    OB["LocalOrderbook"]
    FV["FairValueEngine\nMid-price baseline\nTime-decay adjustment\nInventory signal\nExternal signal hook"]

    T --> FV
    OB -->|mid price| FV

    style T fill:#555,color:#fff
    style OB fill:#555,color:#fff
    style FV fill:#555,color:#fff
```

**Files:**
- `source/fair_value.hpp` / `source/fair_value.cpp`
- `test/source/fair_value_test.cpp`

**Model progression:**

1. **Baseline (v1):** Fair value = orderbook mid-price. No edge, but safe to deploy.
2. **Time-weighted (v2):** Fade extreme prices as resolution approaches. If event closes in 1 hour and price is 95, is that really right?
3. **Inventory-adjusted (v3):** If you're long 80 YES contracts, shade fair value down slightly to encourage selling.
4. **External signal (v4):** Hook for injecting external probability estimates (weather APIs, polling data, etc.).

```cpp
struct FairValueInput {
    double mid_cents;
    double time_to_close_hours;
    int net_position;               // your current inventory
    std::optional<double> external_prob;
};

class FairValueEngine {
public:
    double estimate(const FairValueInput& input) const;  // returns cents [1, 99]
};
```

**Testing:**
- **Unit:** for each model layer, feed known inputs and assert the output is within expected bounds. Test that the baseline returns exactly the mid-price. Test that time-decay pulls extreme values toward 50 as `time_to_close_hours` approaches 0. Test that inventory skew shifts the estimate in the correct direction. Test that output is always clamped to [1, 99].
- **Integration/Contract/ASAN:** N/A — pure math. Run under ASAN to catch any floating-point UB.

---

## Phase 9 — Quoter

The brain of the market maker. Combines fair value, inventory, and risk into bid/ask quotes, then instructs the OrderManager to maintain those quotes on the exchange.

**TDD approach:** Test quote generation in isolation with mocked FairValueEngine and RiskManager. Test that inventory skew shifts quotes correctly. Test that stale quotes are cancelled and refreshed.

```mermaid
graph TD
    OB["LocalOrderbook"]
    FV["FairValueEngine"]
    RM["RiskManager"]
    OM["OrderManager"]
    QE["Quoter\nSpread calculation\nInventory skew\nQuote refresh\nCancel-on-stale"]

    OB --> QE
    FV --> QE
    RM --> QE
    QE --> OM

    style OB fill:#555,color:#fff
    style FV fill:#555,color:#fff
    style RM fill:#555,color:#fff
    style OM fill:#555,color:#fff
    style QE fill:#555,color:#fff
```

**Files:**
- `source/quoter.hpp` / `source/quoter.cpp`
- `test/source/quoter_test.cpp`

**Quoting logic:**

```
fair_value_cents = fair_value_engine.estimate(input)

base_half_spread = max(1, target_spread_cents / 2)
inventory_skew   = net_position * skew_per_contract_cents
                   // positive position = shade down (sell cheaper)

bid = clamp(round(fair_value - base_half_spread - inventory_skew), 1, 98)
ask = clamp(round(fair_value + base_half_spread - inventory_skew), 2, 99)

assert ask > bid  // minimum 1 cent spread
```

**Quote refresh:** On each tick, compare current live quotes to desired quotes. If they differ by more than `reprice_threshold_cents`, cancel and replace. Avoid churning orders on every tick.

```cpp
struct QuoterConfig {
    int target_spread_cents = 4;
    double skew_per_contract_cents = 0.05;
    int reprice_threshold_cents = 1;
    int quote_size = 10;                   // contracts per side
};

class Quoter {
public:
    Quoter(QuoterConfig cfg, FairValueEngine& fv,
           OrderManager& om, RiskManager& rm);

    void update(const std::string& ticker, const LocalOrderbook& ob);
};
```

**Testing:**
- **Unit:** inject fake `FairValueEngine`, `OrderManager`, and `RiskManager`. Assert that `update()` places a bid and ask at the correct prices given a known fair value and zero inventory. Assert inventory skew shifts both quotes in the correct direction. Assert that a quote within `reprice_threshold_cents` of its target is not cancelled and replaced. Assert that risk rejection (fake RM returns false) results in no order.
- **Integration** (`KALSHI_INTEGRATION_TESTS=ON`): run one `update()` cycle against the demo API with a real `LocalOrderbook` populated from REST. Verify a bid and ask appear in the live orderbook. Cancel them.
- **TSAN:** `update()` will be called from the WebSocket callback thread — run under ThreadSanitizer with a concurrent fake feed.

---

## Phase 10 — Main Loop & Integration

Wire all components together. The main loop drives quote updates on each orderbook event.

```mermaid
graph TD
    subgraph Exchange ["Kalshi Exchange"]
        REST["REST API"]
        WS["WebSocket Feed"]
    end

    subgraph Core ["Market Maker Core"]
        AUTH["Auth"]
        HTTP["RestClient"]
        WSCONN["WebSocketClient"]
        OB["LocalOrderbook"]
        OM["OrderManager"]
        RM["RiskManager"]
        FV["FairValueEngine"]
        QE["Quoter"]
        LOOP["Main Loop"]
    end

    REST <-->|signed requests| AUTH
    AUTH --> HTTP
    WS -->|orderbook / fills| WSCONN
    WSCONN --> OB
    WSCONN --> OM
    HTTP --> OM
    OB --> FV
    OB --> QE
    FV --> QE
    RM --> QE
    QE --> OM
    OM --> HTTP
    OM --> RM
    LOOP --> QE
    LOOP --> RM

    style AUTH fill:#555,color:#fff
    style HTTP fill:#555,color:#fff
    style WSCONN fill:#555,color:#fff
    style OB fill:#555,color:#fff
    style OM fill:#555,color:#fff
    style RM fill:#555,color:#fff
    style FV fill:#555,color:#fff
    style QE fill:#555,color:#fff
    style LOOP fill:#555,color:#fff
```

**Main loop pseudocode:**

```cpp
int main() {
    auto auth = Auth{api_key, private_key_pem};
    auto rest = RestClient{auth, std::make_unique<CurlTransport>()};
    auto ws   = WebSocketClient{auth};

    auto ob_map = std::unordered_map<std::string, LocalOrderbook>{};
    auto om     = OrderManager{rest};
    auto rm     = RiskManager{RiskLimits{}};
    auto fv     = FairValueEngine{};
    auto quoter = Quoter{QuoterConfig{}, fv, om, rm};

    // Subscribe to target markets
    for (auto& ticker : target_tickers) {
        auto snap = rest.get_orderbook(ticker);
        ob_map[ticker].apply_snapshot(snap);
        ws.subscribe(ticker);
    }

    ws.on_orderbook_delta([&](const std::string& ticker, Side side, int p, int q) {
        ob_map[ticker].apply_delta(side, p, q);
        quoter.update(ticker, ob_map[ticker]);
    });

    ws.on_fill([&](const Fill& fill) {
        om.record_fill(fill);
        rm.update(om);
    });

    ws.run();  // blocks, drives event loop
}
```

**Testing:**
- **Integration** (`KALSHI_INTEGRATION_TESTS=ON`): full end-to-end run on the demo environment for 60 seconds. Assert the process places quotes, receives at least one orderbook delta, reprices at least once, and shuts down cleanly without open orders (cancel-on-exit).
- **Docker:** build and run inside `Dockerfile.test`. Credentials injected via Docker secrets. This is the form used in CI.
- **ASAN + TSAN:** run the full integration test under each sanitizer. The WebSocket thread + main loop interaction is the highest-risk race condition surface.
- **Backtesting:** record a 10-minute live session to a delta log, replay it through the quoter offline. Compare simulated PnL vs. recorded fills to validate the model.

---

## Post-Phase-10 Roadmap

### Phase 11 — Pluggable Pricing Model

The current `FairValueEngine` is a rule-based heuristic (mid + time-decay + inventory skew + external signal hook). Replace the internals with a `IPricingModel` interface so the estimation strategy is swappable without touching the Quoter or OrderManager.

**Concrete implementations to build toward:**

- **Heuristic** (current): mid-price + parametric time-decay. Ship-safe baseline with no edge.
- **Calibrated statistical model**: train on historical resolution outcomes vs. market price at various time horizons. Logistic regression or XGBoost on features: time-to-close, bid/ask imbalance, recent delta velocity, cross-market correlation. Output is a calibrated probability that feeds as `external_prob`.
- **Order flow model**: short-term signal from trade imbalance and quote stuffing — effective in liquid prediction markets where informed traders leave footprints.
- **Reinforcement learning (long-term)**: directly optimize spread/skew parameters rather than computing a fair value. The Quoter's `QuoterConfig` fields become policy outputs from an RL agent trained on historical PnL.

**Architecture sketch:**

```cpp
class IPricingModel {
public:
    virtual double estimate(const FairValueInput &input) = 0;
    virtual ~IPricingModel() = default;
};

class FairValueEngine {
public:
    explicit FairValueEngine(std::unique_ptr<IPricingModel> model);
    [[nodiscard]] double estimate(const FairValueInput &input) const;
private:
    std::unique_ptr<IPricingModel> model_;
};
```

The `external_prob` field in `FairValueInput` is already the natural injection point for any model that outputs a probability.

---

### Phase 12 — Theo Grid (Fast Repricing for Underlying-Linked Markets)

For markets driven by a fast-moving underlying (BTC price, S&P index, weather score), re-running the pricing model on every tick is too slow and wasteful. Borrow the options market-making technique of pre-computing a **theo grid**: a lookup table of `(underlying_value, time_remaining) → fair_probability`, updated periodically and interpolated linearly between ticks.

**When this matters:**
- Quoting multiple correlated tickers simultaneously (e.g., all BTC price-tier markets: `>$90k`, `>$95k`, `>$99k`). One spot move → reprice all tickers via table lookup with no model calls.
- Underlying moves faster than the model can evaluate (sub-millisecond repricing required).
- Running an ML model whose inference cost is non-trivial.

**Analogy to options greeks:**
- **Delta equivalent**: ∂prob/∂underlying — how much fair value shifts per unit of underlying movement. Drives how quickly quotes must reprice when spot ticks.
- **Theta equivalent**: ∂prob/∂time — already captured by the time-decay layer in FairValueEngine; the grid makes it a free table lookup.

**Architecture sketch:**

```cpp
struct TheoGrid {
    // Precomputed: underlying_prices[i], time_buckets[j] → fair_prob[i][j]
    std::vector<double> underlying_prices;  // e.g. BTC in $1000 increments
    std::vector<double> time_buckets_hours;
    std::vector<std::vector<double>> fair_prob;

    // Called on every underlying tick; O(log n) binary search + O(1) lerp
    double interpolate(double underlying, double time_hours) const;

    // Rebuild the full grid (called on model retrain or param change)
    void rebuild(const IPricingModel &model, const std::string &ticker);
};
```

`TheoGrid::interpolate()` replaces `FairValueEngine::estimate()` on the hot path. The grid is rebuilt in a background thread whenever the model is retrained or market conditions shift significantly.

---

### Phase 13 — Constraint Bitset & Adverse Selection Guard

**Constraint bitset (replaces `bool halted_` in `RiskManager`)**

The current `RiskManager` uses a single boolean halt flag. Replace it with a `std::bitset` where each bit represents a named, independently set/clearable constraint. `is_halted()` becomes `constraints_.any()` — the Quoter interface stays unchanged.

```cpp
enum class Constraint : uint8_t {
    kPnLLimit      = 0,  // daily loss limit breached — requires manual resume()
    kPositionLimit = 1,  // net position too large on a ticker
    kOpenOrders    = 2,  // too many resting orders
    kHighFillRate  = 3,  // adverse selection signal — auto-clears after cooldown
    kStaleBook     = 4,  // no WS delta received within staleness window
    kModelDiverge  = 5,  // FairValueEngine output outside plausible range
    kManualHalt    = 6,  // operator kill switch
    kConnectivity  = 7,  // WS/REST transport failure
};

class RiskManager {
public:
    void   set(Constraint bit);
    void   clear(Constraint bit);
    bool   is_set(Constraint bit) const;
    bool   is_halted() const;           // constraints_.any()

    // Diagnostic: which constraints are active?
    std::string active_constraints() const;
    ...
};
```

Benefits over a single flag:
- Operators can see *why* the system is pulled, not just *that* it is
- Bits with different clearing semantics: `kPnLLimit` requires manual `resume()`; `kStaleBook` auto-clears on the next WS message; `kHighFillRate` auto-clears after a cooldown timer
- Tooling/logging can track constraint transitions over time

**Pull-on-fill-count (adverse selection guard)**

If `N` fills arrive on a ticker within `T` seconds, the system is likely being picked off by an informed trader or stale on news. The correct response is to pull quotes, wait for the orderbook to stabilize, and re-enter after a model refresh.

```cpp
struct AdverseSelectionConfig {
    static constexpr int    kDefaultFillThreshold  = 5;
    static constexpr double kDefaultWindowSeconds  = 30.0;
    static constexpr double kDefaultCooldownSeconds = 10.0;

    int    fill_threshold   = kDefaultFillThreshold;   // fills within window → pull
    double window_seconds   = kDefaultWindowSeconds;
    double cooldown_seconds = kDefaultCooldownSeconds; // before re-entry attempt
};
```

On each `record_fill()` call, the guard checks the rolling window. If the threshold is crossed it sets `Constraint::kHighFillRate` and cancels all quotes for that ticker. The main loop clears `kHighFillRate` after `cooldown_seconds` and triggers a fresh `quoter.update()` cycle.

This fits prediction markets particularly well: information events (news drops, vote counts, weather readings) arrive discretely and move prices step-wise, so the pull-wait-re-enter cycle is a better response than continuous repricing.

---

### Phase 14 — Logging & Observability

**Structured logging (spdlog)**

Every significant event should emit a structured log line — order placed/cancelled, fill received, constraint set/cleared, reprice triggered. Use [spdlog](https://github.com/gabime/spdlog) (already listed as a dependency) with a JSON sink so logs are machine-parseable.

Key log sites:
- `OrderManager::place()` / `cancel()` — order ID, ticker, side, price, qty, latency
- `OrderManager::record_fill()` — fill details, updated net position, realized PnL
- `RiskManager::set(Constraint)` / `clear(Constraint)` — which bit, current full constraint state
- `Quoter::refresh_bid/ask()` — desired vs. current price, reprice decision
- `WebSocketClient` — connect, disconnect, reconnect attempts

**Latency metrics**

The critical latency path is: WS delta received → fair value computed → order sent. Instrument each segment independently so regressions are visible immediately.

Latencies to track (as histograms or rolling percentiles — p50/p95/p99):

| Metric | Definition |
|---|---|
| `ws_delta_to_fv_us` | Time from WS message receipt to `FairValueEngine::estimate()` returning |
| `fv_to_order_us` | Time from `estimate()` returning to HTTP POST sent |
| `order_rtt_ms` | Time from POST sent to 200 response received |
| `fill_to_record_us` | Time from fill WS message to `record_fill()` completing |
| `quote_cycle_us` | Full `quoter.update()` wall time |
| `reprice_rate` | Reprices per minute per ticker (churn indicator) |
| `fill_rate` | Fills per minute per ticker (adverse selection indicator) |

**Implementation sketch**

A lock-free ring-buffer metric collector keeps overhead under 1 µs per sample. A background thread drains it to a Prometheus endpoint or a local CSV file every second.

```cpp
// Minimal interface — no virtual dispatch on the hot path
class Metrics {
public:
    void record(std::string_view name, double value_us);
    void increment(std::string_view name);

    // Called from a background thread to flush samples
    void flush(std::ostream &out);
};
```

The `spdlog` already in the dependency list covers event logging; `Metrics` is a separate lightweight collector. Keep them separate — logs are for debugging, metrics are for dashboards and alerting.

---

### Phase 15 — Config File & Graceful Shutdown

All runtime parameters (target tickers, spread, position limits, API credentials) are loaded from a TOML or JSON config file at startup. A SIGTERM/SIGINT handler cancels all open orders before the process exits.

**Why these together:** Both are required before going live on real capital. Hardcoded parameters mean recompiling to change a ticker; no graceful shutdown means leaving naked quotes on the book after Ctrl-C.

```mermaid
graph TD
    CFG["config.toml"]
    LOADER["load_config\nAppConfig"]
    AUTH["Auth"]
    RC["RestClient"]
    RM["RiskManager"]
    QE["Quoter"]
    OM["OrderManager"]
    SIG["SignalHandler\nSIGTERM / SIGINT"]

    CFG --> LOADER
    LOADER --> AUTH
    LOADER --> RC
    LOADER --> RM
    LOADER --> QE
    SIG -->|cancel_all + stop| OM

    style AUTH fill:#555,color:#fff
    style RC fill:#555,color:#fff
    style RM fill:#555,color:#fff
    style QE fill:#555,color:#fff
    style OM fill:#555,color:#fff
    style CFG fill:#2a6,color:#fff
    style LOADER fill:#2a6,color:#fff
    style SIG fill:#2a6,color:#fff
```

**Files:**
- `source/config.hpp` / `source/config.cpp`
- `test/source/config_test.cpp`
- `config.example.toml`

**Interface:**

```cpp
struct AppConfig {
    std::string api_key;
    std::string private_key_path;
    std::string base_url;
    std::vector<std::string> target_tickers;
    QuoterConfig quoter;
    RiskLimits risk;
};

AppConfig load_config(const std::filesystem::path& path);
```

**Graceful shutdown (in main):**

```cpp
std::signal(SIGTERM, [](int) { g_shutdown.store(true); });
std::signal(SIGINT,  [](int) { g_shutdown.store(true); });

// On shutdown:
om.cancel_all_markets();
ws.stop();
```

**Testing:**
- **Unit:** parse a known fixture, assert every field maps correctly. Test missing required fields throw. Test optional fields get defaults. Test unknown fields are ignored (forward-compat).
- **Unit (shutdown):** send SIGINT to the process under test via `kill(getpid(), SIGINT)`, assert `cancel_all` was called on the fake `OrderManager`.

---

### Phase 16 — CI Pipeline & Coverage

GitHub Actions workflow: build, test, clang-tidy, ASAN, and coverage report on every push and pull request. A coverage threshold fails the build if coverage drops below 80%.

```mermaid
graph TD
    PR["Push / Pull Request"]
    BUILD["cmake --preset=dev\nbuild"]
    TEST["ctest\nall units"]
    TIDY["clang-tidy\nstaged files"]
    ASAN["cmake --preset=asan\nctest"]
    COV["lcov\nHTML report"]

    PR --> BUILD
    BUILD --> TEST
    BUILD --> TIDY
    BUILD --> ASAN
    TEST --> COV

    style PR fill:#2a6,color:#fff
    style BUILD fill:#2a6,color:#fff
    style TEST fill:#2a6,color:#fff
    style TIDY fill:#2a6,color:#fff
    style ASAN fill:#2a6,color:#fff
    style COV fill:#2a6,color:#fff
```

**Files:**
- `.github/workflows/ci.yml`
- `cmake/coverage.cmake`
- `CMakePresets.json` — add `coverage` preset (`-fprofile-arcs -ftest-coverage`)

**CI jobs:**

| Job | Preset | Steps |
|---|---|---|
| `build-and-test` | `dev` | configure → build → ctest |
| `clang-tidy` | `dev` | run tidy on all `.cpp` files |
| `asan` | `asan` | build → ctest under ASAN |
| `coverage` | `coverage` | build → ctest → lcov → upload HTML artifact |

**Testing:**
- The CI itself is the test. Green check on every PR is the deliverable.
- Coverage threshold enforced via `lcov --fail-under-line 80`.

---

### Phase 17 — Benchmarking

Google Benchmark microbenchmarks for the hot path. Gives concrete µs numbers before Phase 12 (Theo Grid) optimization and prevents performance regressions from being silently introduced.

```mermaid
graph TD
    OB["LocalOrderbook"]
    FV["FairValueEngine"]
    QE["Quoter"]
    BM["Google Benchmark\nbench/ targets"]
    OUT["Latency Report\nµs per operation"]

    OB -->|BM_ApplyDelta| BM
    FV -->|BM_FairValueEstimate| BM
    QE -->|BM_QuoterUpdate| BM
    BM --> OUT

    style OB fill:#555,color:#fff
    style FV fill:#555,color:#fff
    style QE fill:#555,color:#fff
    style BM fill:#2a6,color:#fff
    style OUT fill:#2a6,color:#fff
```

**Files:**
- `bench/orderbook_bench.cpp`
- `bench/fair_value_bench.cpp`
- `bench/quoter_bench.cpp`
- `cmake/benchmark.cmake`

**Key benchmarks:**

| Benchmark | What it measures | Target |
|---|---|---|
| `BM_ApplyDelta` | `LocalOrderbook::apply_delta()` on a 100-level book | < 1 µs |
| `BM_ApplySnapshot` | Full snapshot apply | < 10 µs |
| `BM_FairValueEstimate` | `FairValueEngine::estimate()` round trip | < 5 µs |
| `BM_QuoterUpdate` | Full `Quoter::update()` with fake OM/RM | < 50 µs |

**Testing:**
- Benchmarks are not part of `ctest` — run manually with `cmake --build build -t bench`.
- CI job (optional, gated) runs benchmarks and posts results as a PR comment for comparison.

---

### Phase 18 — Replay & Fuzz Testing

Two distinct test capabilities addressing the two highest-risk gaps:

1. **Replay:** Record a live WebSocket session to a newline-delimited JSON file and replay it through the full component stack offline. Validates quoting behavior on real market data without a live connection. Essential for regression-testing pricing changes.
2. **Fuzz:** libFuzzer targets for WebSocket message parsing and JSON deserialization — the most likely crash site from a malformed or unexpected exchange message.

```mermaid
graph TD
    LIVE["Live WebSocket\n(demo env)"]
    REC["record_session\ntool"]
    JSONL["test/fixtures/\nsession_*.jsonl"]
    FAKE["FakeWebSocket"]
    STACK["Full Stack\n(LocalOrderbook + Quoter)"]
    FUZZ["libFuzzer\ntargets"]
    PARSER["WS Message Parser\nJSON Parser"]

    LIVE --> REC
    REC --> JSONL
    JSONL --> FAKE
    FAKE --> STACK
    FUZZ --> PARSER

    style FAKE fill:#555,color:#fff
    style STACK fill:#555,color:#fff
    style PARSER fill:#555,color:#fff
    style REC fill:#2a6,color:#fff
    style JSONL fill:#2a6,color:#fff
    style FUZZ fill:#2a6,color:#fff
```

**Files:**
- `tools/record_session.cpp` — connects to live WS and writes raw messages to `session_<timestamp>.jsonl`
- `test/replay/replay_test.cpp` — drives `FakeWebSocket` from a `.jsonl` file, asserts no crash, no open orders at end
- `test/fuzz/parse_ws_message_fuzz.cpp` — libFuzzer entry point over the WS message parser
- `test/fuzz/parse_json_fuzz.cpp` — libFuzzer entry point over JSON orderbook parsing
- `test/fixtures/session_*.jsonl` — committed recorded sessions (anonymized/demo data)

**Testing:**
- **Replay:** feed a 10-minute demo session; assert no assertions fire, no open orders remain after shutdown, simulated PnL is within plausible bounds.
- **Fuzz (CI):** run with `-max_total_time=30`; any crash or ASAN finding = CI failure. Corpus committed to `test/fuzz/corpus/`.

---

### Phase 19 — Paper Trading Mode

A `--paper` CLI flag wires `PaperTransport` in place of `HttpTransport`. Orders are logged to stdout and a JSON file but not submitted. Fills are simulated when the WebSocket mid-price crosses a resting quote. Lets you run the full system on live market data with zero financial risk.

```mermaid
graph TD
    FLAG["--paper flag"]
    IHT["IHttpTransport"]
    HTTP["HttpTransport\n(production)"]
    PAPER["PaperTransport\n(simulated fills\n+ order log)"]
    OM["OrderManager"]
    LOG["orders.jsonl\nsimulated PnL"]

    FLAG -->|selects| PAPER
    IHT --> HTTP
    IHT --> PAPER
    PAPER --> OM
    PAPER --> LOG

    style IHT fill:#555,color:#fff
    style HTTP fill:#555,color:#fff
    style OM fill:#555,color:#fff
    style FLAG fill:#2a6,color:#fff
    style PAPER fill:#2a6,color:#fff
    style LOG fill:#2a6,color:#fff
```

**Files:**
- `source/paper_transport.hpp` / `source/paper_transport.cpp`
- `test/source/paper_transport_test.cpp`

**Interface:**

```cpp
class PaperTransport : public IHttpTransport {
public:
    // Returns a synthetic 201 with a generated order ID; logs the order
    HttpResponse request(const HttpRequest& req) override;

    // Called by the WS callback when mid crosses a resting quote
    void simulate_fill(const std::string& order_id, int qty);

    const std::vector<Order>& simulated_orders() const;
    double simulated_pnl() const;
};
```

**Testing:**
- **Unit:** verify `place_order` via `PaperTransport` returns a synthetic order ID and logs the order. Verify `cancel_order` removes it from the internal book. Verify `simulated_pnl()` accumulates correctly across a sequence of fills.
- **Manual:** run `--paper` against the demo WS for 10 minutes; verify log output and PnL report are internally consistent.

---

### Phase 20 — Documentation

Doxygen for all public headers. Architecture Decision Records (ADRs) for choices that would otherwise be re-litigated every time someone reads the code.

```mermaid
graph TD
    HDR["source/*.hpp\npublic headers"]
    DOX["Doxygen\ndocs/ target"]
    HTML["docs/html/\nbrowsable API"]
    ADR["docs/adr/\nArchitecture\nDecision Records"]

    HDR --> DOX
    DOX --> HTML

    style HDR fill:#555,color:#fff
    style DOX fill:#2a6,color:#fff
    style HTML fill:#2a6,color:#fff
    style ADR fill:#2a6,color:#fff
```

**Files:**
- `docs/Doxyfile` — targets `source/*.hpp`, outputs to `docs/html/`
- `docs/adr/001-http-client.md` — cpp-httplib over libcurl
- `docs/adr/002-timestamp-parsing.md` — strptime/timegm over `std::chrono::parse`
- `docs/adr/003-websocket-library.md`
- `docs/adr/004-json-library.md`
- `docs/adr/005-tdd-approach.md`
- CMake target `docs` runs Doxygen

**ADR format:**

```markdown
# ADR-NNN: Title
## Status: Accepted
## Context
## Decision
## Consequences
```

**Testing:**
- `cmake --build build -t docs` must succeed with zero warnings in CI.
- New ADR required on any PR that changes a library dependency or core interface.

---

## Dependency Summary

| Library | Purpose | How to add |
|---|---|---|
| OpenSSL | RSA-SHA256 auth signing | `find_package(OpenSSL REQUIRED)` |
| cpp-httplib | HTTP REST client (HTTPS via OpenSSL) | FetchContent `yhirose/cpp-httplib` |
| Boost.Beast | WebSocket client | `find_package(Boost REQUIRED COMPONENTS system)` |
| nlohmann/json | JSON parsing | FetchContent or `find_package` |
| spdlog | Structured logging | FetchContent |
| Google Test | Unit testing (already configured) | Already in `cmake/gtest.cmake` |
| Google Benchmark | Hot-path microbenchmarks | FetchContent `google/benchmark` |
| libFuzzer | Fuzz testing WS/JSON parsers | Clang built-in — `-fsanitize=fuzzer` preset |
| lcov | Test coverage reports | System package `lcov` + `cmake/coverage.cmake` |

---

## Phase Checklist

- [x] Phase 1 — Types & Domain Model
- [x] Phase 2 — Authentication
- [x] Phase 3 — REST Client
- [x] Phase 4 — Local Orderbook
- [x] Phase 5 — WebSocket Client
- [x] Phase 6 — Order Manager
- [x] Phase 7 — Risk Manager
- [x] Phase 8 — Fair Value Engine
- [x] Phase 9 — Quoter
- [x] Phase 10 — Main Loop & Integration
- [ ] Phase 11 — Pluggable Pricing Model
- [ ] Phase 12 — Theo Grid
- [ ] Phase 13 — Constraint Bitset & Adverse Selection Guard
- [ ] Phase 14 — Logging & Observability
- [x] Phase 15 — Config File & Graceful Shutdown
- [x] Phase 16 — CI Pipeline & Coverage
- [x] Phase 17 — Benchmarking
- [x] Phase 18 — Replay & Fuzz Testing
- [x] Phase 19 — Paper Trading Mode
- [x] Phase 20 — Documentation
