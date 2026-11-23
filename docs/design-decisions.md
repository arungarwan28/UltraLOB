# UltraLOB — design decisions

Each entry records the decision, the alternatives considered and the cost accepted.

## 1. Integer tick prices, never floating point

**Decision.** `Price` is `int64_t` ticks (1 tick = 1 cent); conversion happens only at the UI/JSON
boundary.

**Why.** Floating point makes equality, ordering, aggregation and checksums platform-dependent. A
book whose hash depends on compiler flags is not deterministic in any useful sense.

**Cost.** The UI must format ticks; the boundary helpers are the only place that knows about cents.

## 2. Index-based order pool instead of pointers or `std::list`

**Decision.** Orders live in a fixed-capacity array pool with a free list; FIFO links are pool
indices (`prev`/`next`), the ordering index is a hash table of id → slot.

**Alternatives.** `std::list<Order>` (allocation per order, poor locality); `std::deque` per level
(stable references but allocator traffic); pointer-linked intrusive lists (fine until you need to
serialise or copy state).

**Why.** Zero heap traffic in steady state, trivially canonical serialisation, stable layout,
cache-friendly level walks.

**Cost.** An extra indirection through the pool during walks; capacities are compile-time.

## 3. Sorted arrays for price levels instead of `std::map`

**Decision.** Per side, a `std::array<PriceLevel, 4096>` kept sorted best-first.

**Why.** Best price is index 0 (no tree descent), depth export is a contiguous scan, no allocator
involvement, and the array is memmove-friendly because realistic books have hundreds of levels.

**Cost.** Level insertion/removal is O(levels) memmove rather than O(log n). For a book with ~200
levels this is a few hundred bytes of copy; measured `submit_limit_round_trip` is ~32 ns on the
reference machine, so the trade favoured the simpler, allocation-free structure.

## 4. Open-addressing id index with backward-shift deletion

**Decision.** Fixed-size linear-probing hash table, load factor ≤ 50%, no tombstones.

**Why.** Cancels must be O(1) on a book that can hold 131k resting orders. Tombstones degrade under
the add/cancel churn that dominates market data; backward-shift deletion keeps probe chains valid
without them.

**Cost.** ~3 MB of index memory and a hard capacity limit (documented in `config.hpp`).

## 5. The event processor is the only mutator

**Decision.** Nothing calls the engine directly except `EventProcessor::apply`, which owns sequence
validation, duplicate detection and gap policy.

**Why.** Ordering guarantees are a property of the pipeline, not of individual call sites. It also
means the terminal, the replay lab, the benchmark and the chaos runner all exercise identical
semantics.

**Cost.** One extra indirection per event; rejected business actions still consume a sequence number
(that is deliberate: a rejected order is a valid position in the stream).

## 6. Gap policies are explicit, never implicit

**Decision.** `RejectAndHalt`, `RejectAndContinue` (resynchronise, declaring the missing range lost)
and `Buffer` (bounded reordering). The policy is configuration and is reported with every anomaly.

**Why.** Silently skipping a missing sequence is how books diverge from reality without anyone
noticing.

**Cost.** The caller must choose; the default in the console is `Buffer`, which is also what the
replay lab demonstrates.

## 7. Checksum covers book state only

**Decision.** CRC64 over levels, totals, FIFO ids and remaining quantities — not the trade counter,
not the tape.

**Why.** A snapshot image must restore to the same checksum, and the trade tape is display data that
is not serialised. Including a trade counter made restored images unverifiable (this was caught by a
failing test, not by inspection).

**Cost.** Two runs that differ only in trade history hash equal; `Snapshot::equals` separately
compares the trade count.

## 8. Book images are explicit, verifiable and scoped

**Decision.** `encode_book_image` serialises every resting order in the book's own walk order;
`restore_book_image` re-inserts without matching (via `restore_resting_order`) and refuses the image
if the recomputed checksum differs from the header.

**Why.** Snapshot recovery that cannot detect corruption is theatre. Inserting in walk order
reproduces FIFO priority exactly.

**Cost.** The trade tape is not part of the image (documented in `docs/reliability.md`).

## 9. Market orders are IOC

**Decision.** A market order never rests; the unfilled remainder is cancelled. `SubmitResult`
reports `filled`, `remaining` and `rested=false`.

**Why.** Resting a market order at an arbitrary price is a behaviour no venue offers. IOC is the
simplest correct policy and is visible in the UI.

## 10. Modify priority rules

**Decision.** Same price and a quantity decrease keeps queue position; any other modify removes and
re-enters at the tail, matching as a taker if it crosses.

**Why.** This mirrors common venue behaviour (size-down keeps priority, re-price loses it) and is
covered by tests for each branch, including a modify that crosses the book.

## 11. Bounded queues with declared producer policy

**Decision.** SPSC ring buffer; producers use `Retry`, `Block` or `Drop`, each with counters. The
chaos scenario reports retries and counted drops side by side.

**Why.** A bounded queue cannot give unlimited producer progress when the consumer is slower. The
honest engineering answer is to declare and instrument the degradation rather than to claim it
cannot happen.

**Cost.** The queue is single-producer/single-consumer; multi-producer ingestion is out of scope.

## 12. No backend, no database

**Decision.** Every visitor runs their own engine instance in a worker; nothing is persisted.

**Why.** The product demonstrates engine engineering, not service operations. It also keeps the
target cost at zero and removes a shared-state failure mode.

**Cost.** No cross-user market, no accounts, no server-side analytics.

## 13. Separate native and WASM builds, never cross-quoted

**Decision.** CMake/Make build Linux binaries; Emscripten builds the browser module. Reports label
which build they came from.

**Why.** WASM performance is not native performance: different clock source, no pthreads, different
memory model. Publishing one as the other would be a credibility failure.

## 14. Header dependency tracking in the Makefile

**Decision.** `-MMD -MP` plus `-include` of generated `.d` files.

**Why.** During development, changing `MatchingEngine`'s layout left stale objects compiled against
the previous layout; the mixed binary corrupted the heap and reported `free(): invalid next size`.
That is a build-system bug that presents as an engine bug, and it is now impossible to reproduce
without a clean rebuild.

## 15. The baseline engine mirrors the rules, not just the interface

**Decision.** `benchmarks/baseline_engine.hpp` re-implements the trading rules and the canonical
CRC64 walk with `std::map` levels, `std::list` FIFO and `std::unordered_map` ids.

**Why.** A faster engine that also produces a different book is not faster, it is wrong. Sharing the
checksum walk (rather than comparing counts) makes equality provable, and the harness refuses to
report timings when any state check fails.

**Cost.** Two implementations to keep in sync; the baseline is deliberately not optimised beyond
what the standard containers give.

## 16. CRC64 crosses the ABI as a string

**Decision.** `chaos_report_to_json` emits `crc_baseline`/`crc_observed` as JSON strings, matching
the `Snapshot`/`EngineState` fields that already used strings.

**Why.** CRC64 values exceed 2^53; as JSON numbers the browser rounds them, and the console typed
them as strings and crashed on the first render of a chaos report.

**Cost.** Consumers must parse strings for checksums, which is already the convention everywhere
else in the API.

## 17. Requests wait for the worker

**Decision.** `useEngine` keeps a queue of waiters and resolves them when the worker is created;
`request()` awaits that queue instead of failing when no worker exists yet.

**Why.** Child effects run before the parent's effect, so a page that loads data on mount raced the
worker construction and failed with "engine worker not ready". Retrying in the page would have
hidden a lifecycle bug; making readiness explicit fixes every page at once.

**Cost.** One promise per request when the worker is not yet up, and a rejection path on unmount.
