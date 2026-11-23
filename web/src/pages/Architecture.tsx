import { Badge, Panel } from '../components/Panel';

const FLOW = [
  {
    step: '01',
    title: 'Order submitted',
    detail: 'The ticket validates input and converts currency units to integer ticks.',
    tag: 'React',
  },
  {
    step: '02',
    title: 'Message serialised',
    detail: 'A typed postMessage request crosses the thread boundary.',
    tag: 'Web Worker',
  },
  {
    step: '03',
    title: 'Worker receives command',
    detail: 'The engine facade copies the command into the WASM C ABI call.',
    tag: 'TypeScript',
  },
  {
    step: '04',
    title: 'WASM entry point',
    detail: 'ul_submit_limit builds an Event and hands it to the event processor.',
    tag: 'C ABI',
  },
  {
    step: '05',
    title: 'Sequence validated',
    detail: 'Exactly-once ordering, duplicate window, gap policy.',
    tag: 'EventProcessor',
  },
  {
    step: '06',
    title: 'Book updated',
    detail: 'Price levels matched in price-time priority; trades recorded in a ring buffer.',
    tag: 'MatchingEngine',
  },
  {
    step: '07',
    title: 'State dirtied',
    detail: 'Pool, level tables, FIFO links and the rolling trade log change in place.',
    tag: 'Zero allocation',
  },
  {
    step: '08',
    title: 'Snapshot returned',
    detail: 'Flat buffers (prices f64, sizes u64) are read into JS views — no JSON on the hot path.',
    tag: 'Shared buffers',
  },
  {
    step: '09',
    title: 'UI renders',
    detail: 'Ladder, depth chart and tape update; the C++ checksum is displayed as state identity.',
    tag: 'React',
  },
];

const STRUCTURES = [
  {
    name: 'OrderPool',
    detail:
      'Fixed-capacity, index-based free list. Acquire/release are O(1) pointer-free operations, so no allocator work happens in steady state.',
    numbers: '131,072 slots · 64 B per order',
  },
  {
    name: 'LevelTable',
    detail:
      'Sorted array of price levels per side (bids descending, asks ascending). Best price is index 0; insert costs a memmove, which beats pointer chasing in practice.',
    numbers: '4,096 levels per side · zero heap',
  },
  {
    name: 'OrderIndex',
    detail:
      'Open-addressing hash map from order id to pool slot with backward-shift deletion, so cancels stay O(1) under heavy churn without tombstones.',
    numbers: '262,144 buckets · 50% max load',
  },
  {
    name: 'EventProcessor',
    detail:
      'The only component allowed to mutate the engine. Enforces sequential application, duplicate ids and a bounded reorder buffer.',
    numbers: '512-event reorder buffer · 4,096-id duplicate window',
  },
  {
    name: 'SPSC queue',
    detail:
      'Bounded lock-free ring buffer used for feed ingestion, with explicit producer policies: retry, block or drop — and counters for each.',
    numbers: 'Retry policy proven lossless in tests',
  },
  {
    name: 'CRC64 state hash',
    detail:
      'Canonical walk of the book (levels, totals, FIFO order, remaining quantities) hashed with CRC64-ECMA. Replay and snapshot recovery compare this value.',
    numbers: 'Book state only · trades excluded',
  },
];

export function ArchitecturePage() {
  return (
    <div className="flex flex-col gap-4">
      <div className="grid grid-cols-1 gap-4 xl:grid-cols-[minmax(0,1fr)_420px]">
        <Panel
          title="Request path"
          hint="one order, end to end"
          className="min-h-[420px]"
        >
          <ol className="flex flex-col">
            {FLOW.map((item, index) => (
              <li key={item.step} className="relative flex gap-3 pb-3 last:pb-0">
                {index < FLOW.length - 1 && (
                  <span
                    aria-hidden
                    className="absolute left-[13px] top-7 h-[calc(100%-1.25rem)] w-px bg-line"
                  />
                )}
                <span className="num mt-0.5 flex h-[26px] w-[26px] shrink-0 items-center justify-center rounded-md border border-line bg-ink text-[10px] text-mint">
                  {item.step}
                </span>
                <div className="min-w-0">
                  <div className="flex flex-wrap items-baseline gap-2">
                    <h3 className="text-[12px] font-semibold text-chalk">{item.title}</h3>
                    <Badge tone={item.tag === 'React' ? 'violet' : 'neutral'}>{item.tag}</Badge>
                  </div>
                  <p className="mt-0.5 text-[11px] leading-relaxed text-fog">{item.detail}</p>
                </div>
              </li>
            ))}
          </ol>
        </Panel>

        <div className="flex flex-col gap-4">
          <Panel title="Where the work happens">
            <div className="flex flex-col gap-3 text-[11px] leading-relaxed text-fog">
              <div className="rounded-lg border border-mint/25 bg-mint/5 p-3">
                <div className="mb-1 flex items-center gap-2">
                  <Badge tone="mint">browser</Badge>
                  <span className="text-[10px] uppercase tracking-[0.12em] text-fog">
                    same engine, WASM build
                  </span>
                </div>
                <p>
                  Every order, benchmark and chaos scenario on this site is executed by the C++
                  engine compiled with Emscripten — not a JavaScript re-implementation. It runs
                  in a Web Worker so the interface stays responsive, and the build is
                  single-threaded by design.
                </p>
              </div>
              <div className="rounded-lg border border-line bg-ink/60 p-3">
                <div className="mb-1 flex items-center gap-2">
                  <Badge tone="neutral">native</Badge>
                  <span className="text-[10px] uppercase tracking-[0.12em] text-fog">
                    Linux harness
                  </span>
                </div>
                <p>
                  The same sources compile natively with CMake/Ninja for benchmarking, GTest
                  suites, the threaded SPSC tests and the Python parity check. Native and WASM
                  measurements are reported separately — one is not evidence for the other.
                </p>
              </div>
            </div>
          </Panel>

          <Panel title="Truthfulness policy">
            <ul className="flex flex-col gap-2 text-[11px] leading-relaxed text-fog">
              <li>
                <span className="text-mist">No invented numbers.</span> Benchmark tiles show
                either a measured run with its environment or a dash.
              </li>
              <li>
                <span className="text-mist">Not-run is not pass.</span> A test or scenario that
                has not executed is labelled as such.
              </li>
              <li>
                <span className="text-mist">Simulated data, always.</span> The feed is synthetic,
                deterministic and clearly labelled.
              </li>
            </ul>
          </Panel>
        </div>
      </div>

      <Panel title="Data structures" hint="index-based, allocation-free in steady state">
        <div className="grid gap-3 md:grid-cols-2 xl:grid-cols-3">
          {STRUCTURES.map((structure) => (
            <article
              key={structure.name}
              className="flex flex-col gap-1.5 rounded-lg border border-line-soft bg-ink/50 p-3"
            >
              <div className="flex items-center justify-between gap-2">
                <h3 className="num text-[12px] font-semibold text-mint">{structure.name}</h3>
              </div>
              <p className="text-[11px] leading-relaxed text-fog">{structure.detail}</p>
              <div className="num mt-auto pt-1 text-[10px] text-fog/70">{structure.numbers}</div>
            </article>
          ))}
        </div>
      </Panel>

      <div className="grid gap-4 md:grid-cols-2">
        <Panel title="Why indexes instead of pointers">
          <p className="text-[11px] leading-relaxed text-fog">
            Orders are linked into their level's FIFO queue by pool index. Indexed links survive
            copying, are trivially serialisable into a snapshot, keep the checksum canonical, and
            make a restored book image reproduce the original priority exactly. The trade-off is
            an indirection through the pool on each step of a walk — measured, not assumed.
          </p>
        </Panel>
        <Panel title="What is deliberately out of scope">
          <ul className="flex flex-col gap-1.5 text-[11px] leading-relaxed text-fog">
            <li>Real market data, brokerage connectivity, customer funds or order routing.</li>
            <li>Multi-threaded WASM: SharedArrayBuffer and cross-origin isolation are not required here.</li>
            <li>
              Persistence: each visitor gets an independent, in-memory simulation with no backend
              and no database.
            </li>
            <li>IOC market orders and a permissive self-trade policy are documented engine rules.</li>
          </ul>
        </Panel>
      </div>
    </div>
  );
}
