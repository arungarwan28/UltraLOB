# UltraLOB — measured evidence

Generated: 2026-11-23T21:39:08Z (all values are from real runs on this machine)

## Test suite

59 tests, 0 failures — PASS

| suite | tests | failures | duration |
| --- | ---: | ---: | ---: |
| SpscQueue | 1 | 0 | 0.0 ms |
| Backpressure | 2 | 0 | 58.0 ms |
| Chaos | 6 | 0 | 59.0 ms |
| Determinism | 7 | 0 | 114.0 ms |
| MatchingEngine | 15 | 0 | 11.0 ms |
| ObjectPool | 2 | 0 | 0.0 ms |
| Memory | 2 | 0 | 6.0 ms |
| OrderBookLimits | 3 | 0 | 7.0 ms |
| OrderBook | 6 | 0 | 4.0 ms |
| OrderIndex | 3 | 0 | 59.0 ms |
| SequenceGap | 6 | 0 | 35.0 ms |
| WasBindings | 6 | 0 | 54.0 ms |

## Benchmarks (native)

CPU: AMD EPYC 7003 (Milan) · 2 logical cores · Ubuntu 24.04.5 LTS · 13.3.0

| workload | events | repeats | median msg/s | p99 | p99.9 | max | heap allocs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| balanced | 1,000,000 | 3 | 3,898,008 | 521 ns | 1,028 ns | 131,082 ns | 0 |
| balanced | 10,000,000 | 1 | 3,889,979 | 533 ns | 1,037 ns | 16,777,246 ns | 0 |
| burst | 1,000,000 | 3 | 4,782,964 | 282 ns | 525 ns | 131,099 ns | 0 |
| cancel_heavy | 1,000,000 | 3 | 4,804,016 | 285 ns | 528 ns | 131,076 ns | 0 |

Per-event latency samples include timer-read overhead. Throughput is end-to-end and therefore includes feed generation; `matching_events_per_second` excludes it.

## Reliability scenarios

| scenario | result | detection | state matches | detail |
| --- | --- | --- | --- | --- |
| sequence_gap | PASS | 1 | True | seq 101 missing (expected 100): GAP_DETECTED, halted=1, later events refused=1, book unchanged |
| duplicate_event | PASS | 1 | True | replayed event id 50: duplicate, applied-once=200/200, checksum identical |
| out_of_order_event | PASS | 1 | True | seq 61 buffered, seq 60 filled the gap and drained 1 event(s); final book identical to in-order run |
| consumer_slowdown | PASS | 1 | True | retry policy: 3495 retries, 4000/4000 delivered, 0 lost; drop policy: 3812 counted drops; book identical |
| snapshot_recovery | PASS | 1 | True | image of 171 orders at seq 2000 (6912 bytes) restored: checksum verified, resumed 2000 events, state identical to uninterrupted run |
| invalid_order | PASS | 7 | True | 7/7 invalid events rejected, book unchanged, next valid order accepted=1 |

## C++ / Python parity

5 checkpoints compared (5 levels per side), 35862 trades — PASS
