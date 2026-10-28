# Historical Event Ingestion & Mapping Specification

This document specifies the data ingestion formats, validation rules, event taxonomy mappings, and limitations for historical market data in the Hawkes-driven limit order book simulator (`lob::historical`).

---

## 1. Research Questions & Data Requirements

Different market microstructure and statistical questions require fundamentally different data granularities:

| Research Question | Required Data Granularity | Fields Needed | Can Aggregate Bars Support? |
| :--- | :--- | :--- | :--- |
| **Hawkes event clustering & flow dynamics** | Timestamped event sequence (L1/L2) | Timestamp, event type (submit, cancel, execute), side | **NO** (requires discrete arrival times) |
| **Bid-ask spread & depth imbalance** | Multi-level quote snapshots | Timestamp, price levels, displayed quantities | **NO** (requires book state) |
| **Order lifetime distributions** | Order-level lifecycle tracking | Submission timestamp, order ID, terminal status/time | **NO** (must not infer from bars) |
| **Market-order execution cost / slippage** | Market orders + immediate matching trades | Timestamp, aggressor side, trade prices, fill quantities | **NO** |
| **Short-horizon adverse selection** | Passive fills + subsequent quote stream | Resting order side, trade price, post-trade midprices | **NO** |
| **Realized volatility / Price returns** | Periodic midprice or trade prints | Timestamps, prices | **PARTIAL** (trade price returns only) |

> [!WARNING]
> **Prohibition on Speculative Inference**:
> Order queue position, cancellation timing, and individual order lifetimes cannot be inferred from aggregated time bars (e.g. 1-minute OHLCV). If a historical dataset lacks individual order identifiers, those analyses are explicitly flagged as unsupported by the validation report.

---

## 2. Ingestion Format Specification (Version 1)

The canonical interchange formats supported are strict **CSV** and **NDJSON**.

### 2.1 CSV Format
```csv
version,timestamp_ns,source_sequence,event_type,side,price_ticks,quantity_lots,order_id,trade_id
1,1700000000000000000,1,SUBMIT,BUY,100,10,1001,
1,1700000000050000000,2,SUBMIT,SELL,102,10,1002,
1,1700000000100000000,3,EXECUTE,BUY,102,5,1003,5001
1,1700000000150000000,4,CANCEL,BUY,100,10,1001,
```

### 2.2 NDJSON Format
```json
{"version":1,"timestamp_ns":1700000000000000000,"sequence":1,"event_type":"SUBMIT","side":"BUY","price_ticks":100,"quantity_lots":10,"order_id":1001}
{"version":1,"timestamp_ns":1700000000050000000,"sequence":2,"event_type":"SUBMIT","side":"SELL","price_ticks":102,"quantity_lots":10,"order_id":1002}
{"version":1,"timestamp_ns":1700000000100000000,"sequence":3,"event_type":"EXECUTE","side":"BUY","price_ticks":102,"quantity_lots":5,"order_id":1003,"trade_id":5001}
```

### 2.3 Field Semantics & Constraints
- `version`: Integer schema version (currently `1`).
- `timestamp_ns`: Integer nanoseconds from epoch or simulation origin. Must be monotonically non-decreasing.
- `source_sequence`: Monotonically increasing sequence number assigned by the feed or exchange.
- `event_type`: Raw feed event verb (`SUBMIT`, `EXECUTE`, `CANCEL`, `REPLACE`, `STATUS`).
- `side`: Order direction (`BUY` / `B` or `SELL` / `S`).
- `price_ticks`: Integer price ticks (must be $> 0$ for limit orders).
- `quantity_lots`: Integer quantity lots (must be $> 0$).
- `order_id`: Optional unsigned 64-bit identifier for the order.
- `trade_id`: Optional unsigned 64-bit identifier for the trade execution.

---

## 3. Feed Event Mapping to Canonical Taxonomy

Raw feed messages are normalized into the canonical 6-dimensional Hawkes taxonomy:

| Raw Feed Event | Side | Canonical Hawkes Type | Category Index | Meaning |
| :--- | :--- | :--- | :--- | :--- |
| `SUBMIT` / `ADD` / `LIMIT` | Buy | **`LB`** | 0 | Limit order addition on bid |
| `SUBMIT` / `ADD` / `LIMIT` | Sell | **`LA`** | 1 | Limit order addition on ask |
| `CANCEL` / `DELETE` | Buy | **`CB`** | 2 | Cancellation of resting bid |
| `CANCEL` / `DELETE` | Sell | **`CA`** | 3 | Cancellation of resting ask |
| `EXECUTE` / `TRADE` / `FILL` | Buy | **`MB`** | 4 | Marketable buy execution (aggressive buyer) |
| `EXECUTE` / `TRADE` / `FILL` | Sell | **`MS`** | 5 | Marketable sell execution (aggressive seller) |
| `REPLACE` / `MODIFY` | Either | Decomposed | N/A | Decomposed into `Cancel` + `Submit` |
| `STATUS` / `HEARTBEAT` / `HALT` | N/A | Feed Status | N/A | Ignored for Hawkes flow; logged in audit |

### 3.1 Prevention of Double-Counting
1. **Trade Prints vs. Order Executions**: Some feeds (e.g. ITCH/OUCH) emit both an Order Executed message and a separate Trade Summary print. The parser tracks unique `trade_id` values and deduplicates them so an individual trade is not counted multiple times.
2. **Order Replacements / Amendments**: When configured with `ReplaceHandlingPolicy::DecomposeToCancelAndSubmit`, an amendment is represented as a cancellation of the old quantity followed by insertion of the new limit order.

---

## 4. Ingestion Validation Report

The parser emits a comprehensive `IngestionValidationReport` recording:
- `is_valid()`: Pass/fail determination based on hard invariants.
- `timestamp_order_violations`: Count of out-of-order records ($t_k < t_{k-1}$).
- `malformed_records`: Count of records with syntax errors or missing required fields.
- `duplicate_sequences`: Count of duplicate sequence numbers.
- `has_order_level_data` / `has_trade_level_data`: Flags documenting whether order IDs and trade IDs were present in the source feed.
