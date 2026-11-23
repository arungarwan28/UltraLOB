# UltraLOB — architecture

## Components

```
                     ┌───────────────────────────────────────────────┐
 React console       │ Terminal · Book · Performance · Replay ·      │
 (web/src)           │ Reliability · Architecture                    │
                     └───────────────┬───────────────────────────────┘
                                     │ typed postMessage RPC (lib/types.ts)
                     ┌───────────────▼───────────────────────────────┐
 Web Worker          │ engine.worker.ts → UltralobEngine facade      │
 (web/src/workers)   │ (flat buffers, no JSON on the hot path)       │
                     └───────────────┬───────────────────────────────┘
                                     │ C ABI (wasm/bindings.cpp)
                     ┌───────────────▼───────────────────────────────┐
 Engine (C++20)      │ EventProcessor → MatchingEngine → OrderBook   │
 engine/src          │        │                │                     │
                     │        │                ├─ ObjectPool<Order>  │
                     │        │                ├─ LevelTable ×2      │
                     │        │                └─ OrderIndex         │
                     │        └─ ReplaySession + FeedGenerator       │
                     └───────────────────────────────────────────────┘
```

The same `engine/src` translation units are compiled by the Makefile/CMake for Linux and by
Emscripten for the browser. There is no second matching implementation anywhere in the project.

## Order lifecycle

1. `MatchingEngine::submit_limit` validates quantity, price and id uniqueness.
2. `match_incoming` walks the opposite side's best level, consuming FIFO heads until the incoming
   quantity is exhausted or the price no longer crosses.
3. Each execution records a `Trade` in the rolling ring and decrements the maker's remaining
   quantity; a fully-filled maker is unlinked and its pool slot released.
4. The unfilled remainder of a limit order rests at the FIFO tail of its level (creating the level if
   needed). A market order's remainder is cancelled — IOC policy.
5. `SubmitResult` reports accepted/filled/remaining/rested plus a reject reason, never an exception.

## Invariants (asserted by `tests/order_book_test.cpp`)

- Levels are sorted best-first: bids descending, asks ascending; index 0 is always the touch.
- `level.total_qty` equals the sum of `remaining` across the level's FIFO, and `level.order_count`
  equals the FIFO length.
- Every resting order is reachable from its level head through `next` links, in arrival order.
- The sum of level order counts equals `active_orders()` (pool live count = index size).
- Cancel/modify of an unknown id leaves the checksum unchanged.

## Determinism argument

State transitions depend only on the sequence of events:

- Integer prices and quantities; no floating point inside the engine.
- Book iteration order is total: price ordering per side, FIFO ordering inside a level, and pool
  indices are irrelevant to the canonical checksum walk (it follows FIFO links, not slots).
- The feed generator is integer-only xorshift64\*: a `(seed, workload)` pair is a fixed byte stream
  on every platform.
- The checksum (`MatchingEngine::crc64`) walks levels, totals, FIFO ids and remaining quantities in
  that canonical order.

Two engines fed the same stream therefore produce the same `Snapshot` and the same CRC64; the replay
lab's "compare checksums" button runs a second engine over the same feed rather than trusting a
stored value.

## Capacities

| Structure | Capacity | Consequence |
| --- | ---: | --- |
| Orders in pool | 131,072 | submission rejected with `OrderPoolExhausted` beyond this |
| Price levels per side | 4,096 | `BookFull` when the level array is full |
| Trade log | 65,536 | oldest executions roll off the UI tape |
| Reorder buffer | 512 events | buffer overflow is counted and reported |
| Duplicate window | 4,096 ids | direct-mapped, single probe |
| Order id index | 262,144 buckets | max 50% load, backward-shift deletion |

These are compile-time constants in `engine/include/ultralob/config.hpp`. They are sized for an
interactive simulator that must fit comfortably inside a browser-friendly WASM memory.

## WASM boundary

`wasm/bindings.cpp` exposes a C ABI (see `wasm/exports.json`). Results are written into two static
buffers (`u64` and `f64`) and read through typed array views, so no per-call allocation or JSON
parsing happens on the hot path. The layout contract is documented at the top of the bindings file
and mirrored in `web/src/wasm/engine.ts`.

The browser build is deliberately single-threaded (`ALLOW_MEMORY_GROWTH=0`, no pthreads), which keeps
the deployment to static files with no cross-origin isolation requirements.

## Native vs browser

| | Native Linux | Browser WASM |
| --- | --- | --- |
| Purpose | benchmarks, GTest, threaded backpressure, parity | interactive console |
| Threads | yes (SPSC / producer-consumer tests) | no (single-threaded by design) |
| Clock | `std::chrono::steady_clock` (`CLOCK_MONOTONIC`) | `performance.now()` via Emscripten |
| Numbers reported | `reports/benchmark_results.json` | Performance screen, measured at run time |

Numbers are never carried across the two. The Performance screen shows both, labelled separately.

## Hosting topology

```
browser ── GET / ─────────────► Pages edge (static)  index.html
        ── GET /assets/*.js ──► Page    chunk + engine.worker-*.js  (hashed, immutable)
        ── GET /wasm/ultralob.wasm ► application/wasm (pinned by web/public/_headers)
                                   │
                                   └─ Worker: no Functions, no server, no database
```

There is no runtime backend: the worker thread, the WASM module and the bundle are all downloaded once
and then execute entirely client-side. Cloudflare Pages therefore only ever serves six static files
(`index.html`, two JS chunks, one CSS file, the WASM module and `_headers`), which is why the Free plan
is sufficient and why hash-based routing needs no rewrite rules. `_headers` is the only host-specific
configuration in the repository.
