#!/usr/bin/env python3
"""Merge raw UltraLOB measurements into the checked-in report artefacts.

Inputs (produced by `make test`, `scripts/run_benchmarks.sh`, the CLI and the
parity test):

    reports/gtest.xml
    reports/bench_raw_*.json
    reports/chaos_results.json
    reports/parity_results.json

Outputs:

    reports/test_results.json
    reports/benchmark_results.json
    reports/summary.md
"""

from __future__ import annotations

import glob
import json
import os
import statistics
import subprocess
import sys
import xml.etree.ElementTree as ET

REPORTS = "reports"


def load_json(path: str):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def parse_gtest(path: str) -> dict:
    tree = ET.parse(path)
    root = tree.getroot()
    suites = []
    total = 0
    failures = 0

    for suite in root.iter("testsuite"):
        cases = []
        suite_failures = 0
        for case in suite.findall("testcase"):
            status = "PASS"
            if case.find("failure") is not None or case.find("error") is not None:
                status = "FAIL"
                suite_failures += 1
            elif case.get("status") == "notrun":
                status = "NOT RUN"
            cases.append(
                {
                    "name": case.get("name", ""),
                    "status": status,
                    "ms": round(float(case.get("time", "0")) * 1000, 3),
                }
            )
        count = len(cases)
        total += count
        failures += suite_failures
        suites.append(
            {
                "name": suite.get("name", ""),
                "tests": count,
                "failures": suite_failures,
                "ms": round(float(suite.get("time", "0")) * 1000, 3),
                "cases": cases,
            }
        )

    return {
        "suite": "google-test",
        "generated_at": timestamp(),
        "total": total,
        "failures": failures,
        "passed": failures == 0,
        "suites": suites,
    }


def timestamp() -> str:
    result = subprocess.run(
        ["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"], capture_output=True, text=True, check=False
    )
    return result.stdout.strip() or "unknown"


def merge_benchmarks(paths: list[str]) -> dict:
    if not paths:
        return {
            "generated_at": timestamp(),
            "environment": {},
            "engine_limits": {},
            "runs": [],
            "components": [],
            "summary": [],
        }

    payloads = [load_json(path) for path in sorted(paths)]
    environment = payloads[0].get("environment", {})
    limits = payloads[0].get("engine_limits", {})
    runs: list[dict] = []
    components: dict[str, dict] = {}

    for payload in payloads:
        runs.extend(payload.get("runs", []))
        for component in payload.get("components", []):
            components[component["name"]] = component

    grouped: dict[tuple[str, int], list[dict]] = {}
    for run in runs:
        grouped.setdefault((run["workload"], run["events"]), []).append(run)

    summary = []
    for (workload, events), group in sorted(grouped.items()):
        throughput = [run["events_per_second"] for run in group]
        p99 = [run["latency_ns"]["p99"] for run in group]
        summary.append(
            {
                "workload": workload,
                "events": events,
                "repeats": len(group),
                "median_events_per_second": round(statistics.median(throughput)),
                "min_events_per_second": round(min(throughput)),
                "max_events_per_second": round(max(throughput)),
                "throughput_spread_pct": (
                    0.0
                    if len(group) < 2 or statistics.median(throughput) == 0
                    else round(
                        (max(throughput) - min(throughput))
                        / statistics.median(throughput)
                        * 100,
                        2,
                    )
                ),
                "median_p99_ns": round(statistics.median(p99)),
                "median_heap_allocations": round(
                    statistics.median([run["heap_allocations"] for run in group])
                ),
            }
        )

    return {
        "generated_at": timestamp(),
        "environment": environment,
        "engine_limits": limits,
        "runs": runs,
        "components": list(components.values()),
        "summary": summary,
    }


def write_markdown(tests: dict, bench: dict, chaos, parity) -> str:
    lines = ["# UltraLOB — measured evidence", ""]
    lines.append(f"Generated: {timestamp()} (all values are from real runs on this machine)")
    lines.append("")

    lines.append("## Test suite")
    lines.append("")
    if tests["total"] == 0:
        lines.append("NOT RUN — no gtest XML found.")
    else:
        lines.append(
            f"{tests['total']} tests, {tests['failures']} failures — "
            f"{'PASS' if tests['passed'] else 'FAIL'}"
        )
        lines.append("")
        lines.append("| suite | tests | failures | duration |")
        lines.append("| --- | ---: | ---: | ---: |")
        for suite in tests["suites"]:
            lines.append(
                f"| {suite['name']} | {suite['tests']} | {suite['failures']} | {suite['ms']:.1f} ms |"
            )
    lines.append("")

    lines.append("## Benchmarks (native)")
    lines.append("")
    if not bench["runs"]:
        lines.append("NOT RUN — no benchmark JSON found.")
    else:
        env = bench["environment"]
        lines.append(
            f"CPU: {env.get('cpu', 'unknown')} · {env.get('logical_cores', '?')} logical cores · "
            f"{env.get('os', '')} · {env.get('compiler', '')}"
        )
        lines.append("")
        lines.append("| workload | events | repeats | median msg/s | p99 | p99.9 | max | heap allocs |")
        lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for row in bench["summary"]:
            matching = next(
                (
                    run
                    for run in bench["runs"]
                    if run["workload"] == row["workload"] and run["events"] == row["events"]
                ),
                None,
            )
            p999 = matching["latency_ns"]["p99_9"] if matching else 0
            max_ns = matching["latency_ns"]["max"] if matching else 0
            lines.append(
                f"| {row['workload']} | {row['events']:,} | {row['repeats']} | "
                f"{row['median_events_per_second']:,} | {row['median_p99_ns']:,} ns | "
                f"{p999:,} ns | {max_ns:,} ns | {row['median_heap_allocations']} |"
            )
        lines.append("")
        lines.append(
            "Per-event latency samples include timer-read overhead. Throughput is end-to-end "
            "and therefore includes feed generation; `matching_events_per_second` excludes it."
        )
    lines.append("")

    lines.append("## Reliability scenarios")
    lines.append("")
    if not chaos:
        lines.append("NOT RUN — no chaos report found.")
    else:
        lines.append("| scenario | result | detection | state matches | detail |")
        lines.append("| --- | --- | --- | --- | --- |")
        for report in chaos:
            lines.append(
                f"| {report['scenario']} | {'PASS' if report['passed'] else 'FAIL'} | "
                f"{report['detected']} | {report['state_matches']} | {report['detail']} |"
            )
    lines.append("")

    lines.append("## C++ / Python parity")
    lines.append("")
    if not parity:
        lines.append("NOT RUN — no parity report found.")
    else:
        lines.append(
            f"{parity['checkpoints_compared']} checkpoints compared "
            f"({parity['levels_per_side']} levels per side), "
            f"{parity['trades_cpp']} trades — "
            f"{'PASS' if parity['passed'] else 'FAIL'}"
        )
        if parity.get("mismatches"):
            lines.append("")
            for mismatch in parity["mismatches"]:
                lines.append(f"- {mismatch}")
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    os.makedirs(REPORTS, exist_ok=True)

    tests = {"suite": "google-test", "generated_at": timestamp(), "total": 0, "failures": 0,
             "passed": False, "suites": []}
    if os.path.exists(os.path.join(REPORTS, "gtest.xml")):
        try:
            tests = parse_gtest(os.path.join(REPORTS, "gtest.xml"))
        except ET.ParseError as error:
            print(f"could not parse gtest.xml: {error}", file=sys.stderr)
    else:
        print("warning: reports/gtest.xml missing (run `make test`)", file=sys.stderr)

    bench = merge_benchmarks(glob.glob(os.path.join(REPORTS, "bench_raw_*.json")))

    chaos = None
    chaos_path = os.path.join(REPORTS, "chaos_results.json")
    if os.path.exists(chaos_path):
        chaos = load_json(chaos_path).get("scenarios", [])

    parity = None
    parity_path = os.path.join(REPORTS, "parity_results.json")
    if os.path.exists(parity_path):
        parity = load_json(parity_path)

    with open(os.path.join(REPORTS, "test_results.json"), "w", encoding="utf-8") as handle:
        json.dump(tests, handle, indent=2)
        handle.write("\n")

    with open(os.path.join(REPORTS, "benchmark_results.json"), "w", encoding="utf-8") as handle:
        json.dump(bench, handle, indent=2)
        handle.write("\n")

    markdown = write_markdown(tests, bench, chaos, parity)
    with open(os.path.join(REPORTS, "summary.md"), "w", encoding="utf-8") as handle:
        handle.write(markdown)

    print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
