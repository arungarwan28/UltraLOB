#!/usr/bin/env python3
"""Export and summarise a synthetic UltraLOB feed.

The event stream is produced by the engine's own C++ generator (through the CLI),
so analysis can never drift from the stream the benchmark and the browser use.
This script does not re-implement the generator.

    python3 scripts/generate_market_feed.py --events 100000 --seed 7 \
        --workload balanced --out reports/feed_sample.jsonl
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import subprocess
import sys

CLI = os.path.join("build", "ultralob_cli")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--events", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--workload", default="balanced")
    parser.add_argument("--out", default=os.path.join("reports", "feed_sample.jsonl"))
    parser.add_argument("--summary", default=os.path.join("reports", "feed_summary.json"))
    args = parser.parse_args()

    if not os.path.exists(CLI):
        print(f"missing {CLI}: run `make cli` first", file=sys.stderr)
        return 2

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    subprocess.run(
        [
            CLI,
            "replay",
            "--events",
            str(args.events),
            "--seed",
            str(args.seed),
            "--workload",
            args.workload,
            "--checkpoint",
            "0",
            "--events-out",
            args.out,
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    by_type: collections.Counter[str] = collections.Counter()
    by_side: collections.Counter[str] = collections.Counter()
    quantities: list[int] = []
    prices: list[int] = []

    with open(args.out, "r", encoding="utf-8") as handle:
        for line in handle:
            event = json.loads(line)
            by_type[event["type"]] += 1
            if event["type"] == "NEW_ORDER":
                by_side[event["side"]] += 1
                quantities.append(event["qty"])
                if event["order_type"] == "limit":
                    prices.append(event["price"])

    summary = {
        "seed": args.seed,
        "workload": args.workload,
        "events": sum(by_type.values()),
        "by_type": dict(by_type),
        "new_orders_by_side": dict(by_side),
        "quantity": {
            "min": min(quantities) if quantities else 0,
            "max": max(quantities) if quantities else 0,
            "mean": round(sum(quantities) / len(quantities), 2) if quantities else 0,
        },
        "limit_price_range_ticks": [min(prices), max(prices)] if prices else [],
        "note": "Synthetic feed produced by engine/src/feed_generator.cpp (deterministic).",
    }

    with open(args.summary, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)
        handle.write("\n")

    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
