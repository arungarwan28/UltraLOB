#!/usr/bin/env bash
# Baseline vs optimized A/B measurement.
#
# Every invocation of the harness is its own process, so peak RSS and allocation
# counters belong to exactly one implementation. Both engines receive the same
# pre-generated event vector (same seed, same workload, same order distribution).
set -euo pipefail

cd "$(dirname "$0")/.."

EVENTS=${EVENTS:-1000000}
SEED=${SEED:-2024}
REPEATS=${REPEATS:-3}
WORKLOADS=${WORKLOADS:-"balanced cancel_heavy burst"}
OUT_DIR=reports

make -j2 compare >/dev/null

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/cmp_*.json

for workload in $WORKLOADS; do
  for engine in optimized baseline; do
    for repeat in $(seq 1 "$REPEATS"); do
      echo "== $engine / $workload / repeat $repeat =="
      ./build/ultralob_compare \
        --engine "$engine" \
        --workload "$workload" \
        --events "$EVENTS" \
        --seed "$SEED" \
        --repeats 1 \
        --json-out "$OUT_DIR/cmp_${engine}_${workload}_r${repeat}.json" >/dev/null
      # The harness stamps repeat 1 for each process; rewrite the label so the
      # report can group runs by repeat number.
      python3 - "$OUT_DIR/cmp_${engine}_${workload}_r${repeat}.json" "$repeat" <<'PY'
import json, sys
path, repeat = sys.argv[1], int(sys.argv[2])
with open(path) as handle:
    data = json.load(handle)
for run in data["runs"]:
    run["repeat"] = repeat
with open(path, "w") as handle:
    json.dump(data, handle, indent=2)
PY
    done
  done
done

python3 scripts/compare_report.py
