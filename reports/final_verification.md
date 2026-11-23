# UltraLOB — final verification and release report

Generated 2026-11-23 from real runs on the build machine (AMD EPYC 7003, 2 logical cores,
Ubuntu 24.04.5, GCC 13.3.0, Emscripten `latest`, Node 20, Chromium via Playwright).

Every row below is either **PASS** (executed and observed), **FAIL** (executed and failed) or
**NOT VERIFIED** (could not be executed here; the reason is stated). Nothing is inferred.

---

## 1. Browser verification matrix

Browser: Chromium (Playwright) against the Vite dev server and, separately, against the production
bundle in `web/dist`. Viewports: 1440×900 desktop and 390×844 mobile.

| # | Page | Renders | Interactive controls | Data from real WASM | Console / network | 390 px layout | Verdict |
| --- | --- | :---: | --- | :---: | :---: | :---: | :---: |
| 01 | Live Trading Terminal | yes | seed book, reset, buy/sell, limit/market, price, quantity, quick qty 10/25/50/100, submit, ladder click-to-quote | yes — resting/executions/events come from `ul_*` calls | no errors, no 4xx/5xx | no horizontal overflow, header wraps | **PASS** |
| 02 | Order Book Visualizer | yes | impact-probe size, buy/sell probe, execute probe, refresh | yes — depth, imbalance, fills and average price from the engine | no errors | no overflow | **PASS** |
| 03 | Performance Explorer | yes | workload ×3, event count ×4, seed, run benchmark | yes — measured inside the WASM worker (1.51 M msg/s at 100 k) | no errors | no overflow | **PASS** |
| 04 | Deterministic Replay Lab | yes | events, seed, workload ×3, gap policy ×3, load, step 1/100/1k, finish, compare checksums | yes — 20,000/20,000 applied, 14,200 executions, live CRC == reference CRC | no errors | no overflow | **PASS** |
| 05 | Reliability / Chaos Lab | yes | run all (auto-runs on mount) | yes — 6/6 scenarios, counters and CRCs from `chaos_report` | no errors | no overflow | **PASS** |
| 06 | Architecture Explorer | yes | static content, 9-step request path, truthfulness policy | n/a (explanatory) — live header counters still come from the engine | no errors | no overflow | **PASS** |

Navigation between all six pages was exercised in both directions, including a full page reload on
`#/reliability` (hash routing) — the page re-mounts and re-runs its scenario set.

Evidence screenshots: `artifacts/verify-01-terminal-desktop.png` … `verify-08-mobile-book.png`.

Observed values worth recording (all produced by the WASM build):

- Terminal after "seed book": best bid 99.99 / best ask 100.01, 13–20 resting orders, 0 rejects;
  a crossing limit produced executions and the ladder quote persisted across a side switch.
- Order book probe of 250 lots: filled 250 at average 100.14, +0.03 vs touch.
- Performance, 100 k balanced: 1.51 M msg/s end-to-end, p50 and p99 `< 1 µs` (browser timer
  resolution), p99.9 65.6 µs. 1 M balanced: 1.31 M msg/s in 761 ms, 743,317 trades,
  CRC64 15894895422671397000.
- Replay: 20,000 events, 14,200 executions, 0 gaps, live and independent reference run both
  CRC64 10344118183512226336.
- Reliability: 6/6 PASS — sequence gap (halted, book unchanged), duplicate event (applied once,
  identical checksum), out-of-order (buffered and drained, identical), consumer slowdown
  (3,495 retries, 4,000/4,000 delivered; 3,812 counted drops under the drop policy, book identical),
  snapshot recovery (171 orders / 6,912 bytes restored and verified), invalid order (7 rejects,
  book untouched).

One console message appears only when the **built** bundle is served through the chat preview proxy:
a failed `wss://<preview-host>/` handshake from the proxy's live-reload client. It is not present in
the application bundle (no WebSocket code in `web/src` or `web/dist`) and does not occur for a normal
static host.

## 2. Engineering code review — findings and fixes

| Area | Finding | Action |
| --- | --- | --- |
| WASM ABI | `ul_trades` wrote into a 600-entry window although the trade ring holds up to 1,365 entries per call; a larger `limit` overflowed the caller buffer | fixed-size `kOutSlots/6` staging buffer with clamping; regression test drives a >600-trade window |
| WASM ABI | `ul_out_u64` declared `uint64_t*` in the header but `unsigned long long*` in the definition — broke the native build of the ABI test | signatures unified |
| WASM ABI | `ul_chaos_all_json` formatted scenario details with unbounded `sprintf` into a fixed buffer, and did not escape quotes | bounds-checked append helper with JSON escaping in `engine/src/chaos.cpp`; test asserts balanced brackets |
| WASM ABI | dead `ul_replay_snapshot_crc` export | removed (38 exports remain, matching `wasm/exports.json`) |
| WASM stack | 64 KiB default stack silently corrupted the JSON buffer, which surfaced as `Unexpected end of JSON input` in the Reliability page | `-sTOTAL_STACK=2097152 -sSTACK_OVERFLOW_CHECK=2` in the Makefile and CMake; root cause confirmed, not masked |
| Order book | `add()` stored the caller's `Order` by value without adopting it into the pool record, so later mutations diverged | pool record is authoritative; caller fields are adopted on insert |
| Order book | if the id-index insert failed after a level was created, an empty level was left behind | rollback of the newly created level |
| Order book | `remove()` re-derived state from the caller's handle, so a stale copy could corrupt FIFO links | removal re-reads the pool record and uses `handle.slot` |
| Order book | an O(levels) "defensive" scan on every remove cost ~30% throughput (found by the A/B harness) | removed; correctness kept by the pool-record fix and asserted by `order_book_limits_test` |
| Book image | record count was written after the records, and a short/mismatched image could be partially applied | count computed first, overflow guard, restore refuses mismatched images |
| CRC64 | `crc64_update_u64` relied on host endianness | explicit little-endian byte encoding |
| Chaos | snapshot scenario captured the order count at the wrong moment; `SpscQueue` (~14 KiB) lived on the stack | count captured at checkpoint; queues heap-allocated |
| Chaos API | CRC64 values were emitted as JSON numbers, which exceed JavaScript's safe integer range and crashed the Reliability page (`crc.slice is not a function`) | emitted as JSON strings; ABI test asserts the quoted form |
| Worker lifecycle | a failed WASM load left `ready` cached, and pending promises were never settled on unmount | worker clears the cache on load failure; `useEngine` rejects pending work on cleanup |
| Worker readiness | pages that fetch on mount (Reliability) ran their effect before the worker existed and failed with "engine worker not ready" | requests now await a worker-ready queue inside `useEngine` |
| Terminal state | `remaining` was inferred in the UI instead of read from the engine | taken from the engine snapshot |
| Ladder quoting | clicking a price did not reach the ticket; switching side silently re-quoted the touch | end-to-end `onPriceSelect` → `quotedPrice` + nonce, `pinned` keeps the chosen price |
| Latency display | 0 ns rendered literally even though browser timers cannot resolve below ~1 µs | `< 1 µs` plus a panel note |
| Responsive | 38–107 px horizontal overflow at 390 px on Terminal, Book and Performance | panel headers wrap, page grids are explicitly single-column below `xl`, the level table scrolls inside its panel |

Test coverage added with the fixes: `tests/order_index_test.cpp` (300,000-operation differential
against `std::unordered_map`), `tests/order_book_limits_test.cpp` (phantom level, level table full,
remove semantics), `tests/wasm_bindings_test.cpp` (ABI layout, >600-trade window, chaos array
balance, CRC-as-string).

## 3. Baseline versus optimised benchmark (native Linux)

See [`baseline_comparison.md`](baseline_comparison.md) for the full table, methodology and charts.
Same pre-generated event vector for both engines, identical seed/workload/distribution, 20,000-event
warm-up on a throwaway instance, one engine per process for attributable peak RSS.

| workload | UltraLOB msg/s | baseline msg/s | ratio | p99 | p99.9 | max | allocations | peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| balanced | 4.70 M | 2.97 M | 1.58× | 516 / 2,049 ns | 537 / 4,098 ns | 131 µs / 16.8 ms | 0 / 1,013,860 | 71.1 / 116.1 MiB |
| burst | 4.71 M | 4.10 M | 1.15× | 513 / 521 ns | 530 / 2,074 ns | 131 µs / 16.8 ms | 0 / 881,876 | 71.1 / 113.0 MiB |
| cancel heavy | 5.01 M | 3.67 M | 1.37× | 286 / 537 ns | 532 / 2,077 ns | 131 µs / 16.8 ms | 0 / 1,034,732 | 71.1 / 114.4 MiB |

State equality (final CRC64, CRC64 at 25/50/100 %, trade count, rejects, active orders, depth, last
200 trades) is **PASS** for all three workloads. The 16.8 ms baseline maxima are scheduler
preemption on a shared 2-vCPU runner; they are reported, not trimmed.

## 4. Native and WASM results

| | Native Linux | Browser (WASM) |
| --- | --- | --- |
| GoogleTest | 59/59 in 12 suites — PASS | not run as a suite; validated through the ABI test and the console |
| Throughput, balanced 1 M | 3.90 M msg/s (median of 3) | 1.31 M msg/s (single run, 1 M) |
| Throughput, balanced 100 k | — | 1.51 M msg/s |
| p99 | 521 ns | `< 1 µs` (timer resolution) |
| p99.9 | 1.03 µs | 65.6 µs |
| Heap allocations in loop | 0 | not instrumented (Emscripten allocator) |
| C++/Python parity | 50,000 events, crc64 7009270412079057528, 35,862 trades, 2,873 rejects | n/a |
| Chaos scenarios | 6/6 PASS | 6/6 PASS |

Never cross-quoted: the native table comes from `reports/benchmark_results.json`, the WASM numbers
from the Performance screen's own clock inside the worker.

## 5. Deployment

**BLOCKED — no credentials in this environment.** `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`
are absent from the environment, `npx wrangler whoami` answers "You are not authenticated", the git
repository has **no remote** (0 remotes) and no version tag. No public URL exists and none is claimed.
`api.cloudflare.com` is reachable from this sandbox (it answers 403 *missing auth header*, not a
network block), so the only missing ingredient is the token pair. Cloudflare Pages Free needs no card
and no domain: the `*.pages.dev` hostname would be the public URL.

What was verified instead, against the real production bundle (`web/dist`) served over HTTP:

- `npm run build` — PASS (typecheck + bundle, 1.4 MB total, ≈407 KB gzipped JS).
- Asset contract — `/` 200 `text/html`, `/wasm/ultralob.wasm` 200 `application/wasm`,
  worker chunk 200 `text/javascript`; no 4xx/5xx for any request during the session.
- Fresh session: engine boots, "seed book" produces 44 resting orders from the real WASM build.
- Deep link + refresh on `#/reliability` re-mounts and reports 6/6 PASSING.
- Hash routing means refresh works without SPA rewrite rules.

Pre-flight checked on the built bundle: 6 files, `wasm/ultralob.wasm` 54,810 B
(md5 `3dee8f4d14db21ba17937535d01bc31c`, byte-identical to `web/public`), `dist/_headers` pinning
`application/wasm`, `assets/engine.worker-B72_JMnr.js` referenced by `index-BpQNRdRc.js`, all
`/assets/*` returning 200, and no credential-like string anywhere in `dist`. The deploy workflow now
asserts the same set (plus the 20,000-file cap) before it publishes, and is gated to pushes on `main`
with a single-concurrency `pages-production` group.

To publish: run `cd web && npm ci && npm run build`, then
`npx wrangler pages deploy web/dist --project-name=ultralob` with those two secrets (or push to
`main` and let `.github/workflows/deploy.yml` do it), then re-run the six-page matrix above against
the returned URL. Only then tag `v1.0.0`.

## 6. Known limitations

- No public deployment was performed or verified (see above).
- WASM timings use `performance.now()`, which is clamped; p50/p99 below ~1 µs cannot be resolved.
- Native numbers come from a shared 2-vCPU runner without pinning; run-to-run throughput varies by
  ~25%, so ratios within one session are meaningful and absolute values across sessions are not.
- The baseline engine is one naive variant, not a survey of alternatives.
- Heap allocation counting exists only natively; the browser reports no allocation figures.
- Snapshot restore covers book state, not the rolling trade tape (documented policy).
- Responsive checks covered 390×844 and 1440×900 only; tablets and landscape phones were not tested.
- GoogleTest is not built for WebAssembly; the WASM surface is covered by the native ABI test plus
  the browser matrix.

## 7. Prioritised follow-ups

1. Deploy to Cloudflare Pages with the documented secrets and re-run the browser matrix against the
   public HTTPS URL in a clean profile.
2. Add `perf_event_open`-based cache counters on a pinned host for cache-behaviour claims.
3. Instrument allocations in WASM (`emscripten` allocator hooks) so browser runs can report them.
4. Add a Playwright smoke suite (six pages, worker boot, replay comparison) to CI so the manual
   matrix cannot regress silently.
5. Split the ECharts bundle (≈400 KB gzipped total) or lazy-load the Performance/Book charts.
6. Extend responsive checks to 768 px and landscape and add a touch-target audit.
