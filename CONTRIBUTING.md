# Contributing to UltraLOB

## Ground rules

1. **Correctness before speed.** A change that makes the engine faster but breaks determinism,
   reject semantics or checksum stability is not acceptable.
2. **Every feature ships with evidence.** Implementation + a test (or a verifiable measurement) +
   documentation. If something has not been run, it is marked *NOT RUN* — never "passed".
3. **No fabricated numbers.** Report measurements with their environment, and never quote the
   native numbers as if they were browser numbers (or vice versa).

## Development loop

```bash
make test                                   # must stay green
python3 tests/parity_test.py --events 50000 # C++ vs Python book state
cd web && npm run typecheck && npm run build
```

Before touching the engine's public layout or the WASM ABI, rebuild both targets:

```bash
make clean && make -j2 test cli bench
make wasm        # requires an activated emsdk
```

The Makefile tracks header dependencies (`-MMD -MP`). Stale objects compiled against an old class
layout are undefined behaviour, not a build annoyance — the dependency files exist because that
failure mode was hit once and diagnosed.

## Coding conventions

- C++20, no exceptions or RTTI in the engine, no heap allocation after construction.
- Integer prices in ticks; never floats inside the engine.
- Index-based links in pooled structures (no raw pointers into pools).
- Every policy decision that a venue would document (IOC, modify priority, self-trade) belongs in a
  comment next to the code **and** in `docs/design-decisions.md`.
- Web: TypeScript strict mode, no `any` in new code, numbers formatted through `lib/format.ts`.

## Adding a chaos scenario

1. Implement it in `engine/src/chaos.cpp` (thread-free so it also runs in WASM).
2. Add an assertion to `tests/chaos_test.cpp`.
3. It appears automatically in the CLI (`ultralob_cli chaos`), the reliability lab and
   `reports/summary.md`.

## Commit style

Short imperative subject, then *why* it changed. Reference the invariant or measurement that
motivated it.
