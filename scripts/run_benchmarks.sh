#!/usr/bin/env bash
# Run the native benchmark matrix and regenerate reports/ from raw output.
set -euo pipefail
cd "$(dirname "$0")/.."

make cli bench >/dev/null

mkdir -p reports
rm -f reports/bench_raw_*.json

echo "== balanced, 1M events, 3 repeats =="
./build/ultralob_bench --events 1000000 --seed 2024 --workload balanced --repeats 3 \
  --json-out reports/bench_raw_balanced_1m.json >/dev/null

echo "== balanced, 10M events, 1 repeat =="
./build/ultralob_bench --events 10000000 --seed 2024 --workload balanced --repeats 1 \
  --json-out reports/bench_raw_balanced_10m.json >/dev/null

echo "== cancel heavy, 1M events, 3 repeats =="
./build/ultralob_bench --events 1000000 --seed 2024 --workload cancel_heavy --repeats 3 \
  --json-out reports/bench_raw_cancel_1m.json >/dev/null

echo "== burst, 1M events, 3 repeats =="
./build/ultralob_bench --events 1000000 --seed 2024 --workload burst --repeats 3 \
  --json-out reports/bench_raw_burst_1m.json >/dev/null

echo "== reliability scenarios =="
./build/ultralob_cli chaos --json-out reports/chaos_results.json >/dev/null

echo "== C++/Python parity =="
python3 tests/parity_test.py --events 50000 --seed 7 --workload balanced >/dev/null

echo "== merging reports =="
python3 scripts/generate_report.py >/dev/null
echo "wrote reports/benchmark_results.json, reports/test_results.json, reports/summary.md"

echo "== refreshing the console's checked-in native snapshot =="
python3 - <<'PY'
import json
with open('reports/benchmark_results.json') as handle:
    data = json.load(handle)
with open('web/src/data/native-benchmark.json', 'w') as handle:
    json.dump(data, handle, indent=2)
    handle.write('\n')
print('web/src/data/native-benchmark.json updated')
PY
