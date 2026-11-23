# UltraLOB — browser verification

Every claim here comes from driving the console in Chromium (Playwright) against the real WASM
build. Screenshots are in [`../artifacts/`](../artifacts). Full PASS/FAIL/NOT VERIFIED report:
[`../reports/final_verification.md`](../reports/final_verification.md).

## Matrix

| Page | Route | Controls exercised | Data source | Verdict |
| --- | --- | --- | --- | --- |
| Live Trading Terminal | `#/terminal` | seed book, reset, side, order type, price, quantity, quick quantities, submit limit/market, ladder click-to-quote | `ul_submit_*`, `ul_snapshot` | PASS |
| Order Book Visualizer | `#/book` | probe size, probe side, execute probe, refresh | `ul_snapshot`, `ul_trades`, `ul_impact_probe` | PASS |
| Performance Explorer | `#/performance` | 3 workloads, 4 event counts, seed, run benchmark | `ul_bench_run` inside the worker | PASS |
| Deterministic Replay Lab | `#/replay` | events, seed, workload, gap policy, load, step 1/100/1k, finish, compare checksums | `ul_replay_*` | PASS |
| Reliability / Chaos Lab | `#/reliability` | run all | `ul_chaos_all_json` | PASS |
| Architecture Explorer | `#/architecture` | static walkthrough | explanatory; header counters live | PASS |

## What was actually observed

- **Terminal** — seeding produced a real two-sided book (bid 99.99 / ask 100.01, 13–20 resting
  orders). A crossing limit produced executions; a cancel reduced resting orders; a market order was
  IOC (never rested); "reset" cleared the book; an invalid quantity produced the client-side message
  without an engine call. Clicking 100.01 in the ladder filled the ticket price and it survived a
  side switch.
- **Order book** — depth chart drawn from engine levels, imbalance computed from level totals, probe
  of 250 lots filled at an average of 100.14 (+0.03 against the touch).
- **Performance** — 100 k balanced: 1.51 M msg/s, p50/p99 `< 1 µs`, p99.9 65.6 µs. 1 M balanced:
  1.31 M msg/s in 761 ms, 743,317 trades, CRC64 15894895422671397000.
- **Replay** — 20,000 events, 14,200 executions, 0 gaps; "compare checksums" ran an independent
  second engine over the same stream: both 10344118183512226336 at sequence 20,000.
- **Reliability** — 6/6 PASS, including consumer slowdown with 3,495 retries and 4,000/4,000
  delivered, 3,812 counted drops under the drop policy, and a 171-order / 6,912-byte snapshot
  restored with a verified checksum.

## Console and network

No JavaScript errors and no failed requests on any page. The only non-application message is a
failed `wss://<preview-host>/` handshake emitted by the chat preview proxy's live-reload client when
the static bundle is served through it; there is no WebSocket code in `web/src` or `web/dist`.

## Responsive

At 390×844 every page reports `documentElement.scrollWidth <= innerWidth` and no clipped buttons;
panels stack, panel headers wrap, the level table scrolls inside its panel. At 1440×900 the three
column layouts are used for Terminal, Book and Replay. Tablets and landscape were not tested.

All of the above was measured on the release candidate served locally (production bundle over HTTP and
the Vite dev server), not on a public URL. Nothing here is a deployment verification.

## Deployment pre-flight — verified, not deployed

Checked against the real `web/dist` after `npm run build` (exit 0, 9.29 s, `tsc --noEmit` included):

| Check | Result | Evidence |
| --- | --- | --- |
| Bundle complete | PASS | 6 files: `index.html`, `_headers`, `assets/index-*.js`, `assets/index-*.css`, `assets/engine.worker-*.js`, `wasm/ultralob.wasm` |
| WASM artefact | PASS | `wasm/ultralob.wasm` = 54,810 B, md5 `3dee8f4d14db21ba17937535d01bc31c`, identical to `web/public` |
| MIME configuration | PASS | `web/public/_headers` → `/wasm/*` `Content-Type: application/wasm`; copied into `dist/_headers` |
| Worker chunk emitted | PASS | `index-*.js` references `assets/engine.worker-B72_JMnr.js`, which serves 200 |
| Assets resolve | PASS | every `/assets/…` referenced by `index.html` returns 200 from a static server |
| No secrets in bundle | PASS | no `CLOUDFLARE_*`, token or key material in `dist` |
| Bundle size | PASS | 1.4 MB total, ≈407 KB gzipped JS — inside the 25 MiB per-file and 20,000-file Free limits |
| Wrangler auth | **BLOCKED** | `wrangler whoami` → "You are not authenticated" |
| Public URL | **NOT VERIFIED** | none exists; `CLOUDFLARE_API_TOKEN` / `CLOUDFLARE_ACCOUNT_ID` are not present in the environment |

To finish, set `CLOUDFLARE_API_TOKEN` (Pages: Edit) and `CLOUDFLARE_ACCOUNT_ID` in the project secret
store, then run `npx wrangler pages deploy web/dist --project-name=ultralob` and repeat this matrix
against the returned `*.pages.dev` URL in a clean profile.
