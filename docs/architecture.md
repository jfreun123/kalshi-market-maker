# Architecture

## Data Flow

```mermaid
sequenceDiagram
    participant WS as WebSocket
    participant OB as LocalOrderbook
    participant FV as FairValueEngine
    participant QE as Quoter
    participant RM as RiskManager
    participant OM as OrderManager
    participant REST as RestClient

    WS->>OB: orderbook_delta(side, price, qty)
    OB->>FV: mid_price()
    FV->>QE: estimate(input) → fair_value_cents
    QE->>RM: check_order(ticker, side, price, qty)
    RM-->>QE: approved
    QE->>OM: cancel(stale_bid_id)
    OM->>REST: DELETE /orders/{id}
    QE->>OM: place(ticker, Yes, new_bid, qty)
    OM->>REST: POST /orders
```

## Key Design Decisions

- **Interface + fake pattern** — `IHttpTransport`, `IWebSocket` hide all I/O. Unit tests inject fakes; integration tests use real implementations.
- **Event-driven quoting** — quotes refresh on orderbook deltas, not a timer. Acts only on new information.
- **Inventory skew over flattening** — Quoter shifts bid/ask symmetrically around fair value based on net position rather than placing aggressive orders to flatten.
- **RiskManager is pure in the hot path** — `check_order()` is side-effect-free. Only `halt()` mutates state.
