#!/usr/bin/env python3
"""Merge baseline-vs-optimized A/B runs into a report, charts and verdict.

Inputs:  reports/cmp_<engine>_<workload>_r<N>.json (one per process)
Outputs: reports/baseline_comparison.json
         reports/baseline_comparison.md
         reports/charts/{throughput,p99,p999,max,allocations,rss}.svg

Correctness gate: both engines must agree on the final book checksum, the
checksums at 25/50/100% of the stream, trade count, reject count, active order
count, depth and the trade tail. A disagreement fails the script (exit 1) so it
can never be reported as a performance result.
"""

from __future__ import annotations

import glob
import json
import pathlib
import statistics
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORTS = ROOT / "reports"
CHARTS = REPORTS / "charts"

ENGINES = ("optimized", "baseline")
LABELS = {"optimized": "UltraLOB (optimized)", "baseline": "Baseline (std::map/list)"}


def load_runs() -> dict[str, dict[str, list[dict[str, Any]]]]:
    grouped: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for path in sorted(glob.glob(str(REPORTS / "cmp_*.json"))):
        with open(path) as handle:
            data = json.load(handle)
        engine = data["engine"]
        for run in data["runs"]:
            run["environment"] = data.get("environment", {})
            run["generated_at"] = data.get("generated_at", "")
            grouped.setdefault(run["workload"], {}).setdefault(engine, []).append(run)
    return grouped


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def summarise(runs: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "repeats": len(runs),
        "median_events_per_second": median([r["events_per_second"] for r in runs]),
        "median_matching_events_per_second": median(
            [r["matching_events_per_second"] for r in runs]
        ),
        "median_elapsed_ms": median([r["elapsed_ms"] for r in runs]),
        "median_p50_ns": median([r["latency_ns"]["p50"] for r in runs]),
        "median_p99_ns": median([r["latency_ns"]["p99"] for r in runs]),
        "median_p99_9_ns": median([r["latency_ns"]["p99_9"] for r in runs]),
        "median_p99_99_ns": median([r["latency_ns"]["p99_99"] for r in runs]),
        "median_max_ns": median([r["latency_ns"]["max"] for r in runs]),
        "median_mean_ns": median([r["latency_ns"]["mean"] for r in runs]),
        "median_heap_allocations": median([r["heap_allocations"] for r in runs]),
        "median_allocated_bytes": median([r["allocated_bytes"] for r in runs]),
        "median_peak_rss_kb": median([r["peak_rss_kb"] for r in runs]),
        "trades": median([r["trades"] for r in runs]),
        "rejected": median([r["rejected"] for r in runs]),
        "active_orders": median([r["active_orders"] for r in runs]),
    }


def correctness(opt: list[dict[str, Any]], base: list[dict[str, Any]]) -> dict[str, Any]:
    by_repeat_opt = {r["repeat"]: r for r in opt}
    by_repeat_base = {r["repeat"]: r for r in base}
    shared = sorted(set(by_repeat_opt) & set(by_repeat_base))
    checks = {
        "crc64": True,
        "trade_count": True,
        "rejected": True,
        "active_orders": True,
        "depth": True,
        "trade_tail_crc": True,
        "checkpoints": True,
    }
    mismatches: list[str] = []

    for repeat in shared:
        a = by_repeat_opt[repeat]
        b = by_repeat_base[repeat]
        if a["crc64"] != b["crc64"]:
            checks["crc64"] = False
            mismatches.append(f"repeat {repeat}: book crc {a['crc64']} != {b['crc64']}")
        if a["trades"] != b["trades"]:
            checks["trade_count"] = False
            mismatches.append(f"repeat {repeat}: trades {a['trades']} != {b['trades']}")
        if a["rejected"] != b["rejected"]:
            checks["rejected"] = False
            mismatches.append(f"repeat {repeat}: rejects {a['rejected']} != {b['rejected']}")
        if a["active_orders"] != b["active_orders"]:
            checks["active_orders"] = False
            mismatches.append(
                f"repeat {repeat}: active {a['active_orders']} != {b['active_orders']}"
            )
        if (a["depth_bid"], a["depth_ask"]) != (b["depth_bid"], b["depth_ask"]):
            checks["depth"] = False
            mismatches.append(
                f"repeat {repeat}: depth {a['depth_bid']}/{a['depth_ask']} != "
                f"{b['depth_bid']}/{b['depth_ask']}"
            )
        if a["trade_tail_crc"] != b["trade_tail_crc"]:
            checks["trade_tail_crc"] = False
            mismatches.append(f"repeat {repeat}: trade tail differs")
        ca = [(c["events"], c["crc64"]) for c in a["checkpoints"]]
        cb = [(c["events"], c["crc64"]) for c in b["checkpoints"]]
        if ca != cb:
            checks["checkpoints"] = False
            mismatches.append(f"repeat {repeat}: checkpoint checksums differ")

    return {
        "shared_repeats": shared,
        "checks": checks,
        "identical": bool(shared) and all(checks.values()),
        "mismatches": mismatches,
    }


def bar_chart(
    path: pathlib.Path,
    title: str,
    workloads: list[str],
    series: dict[str, list[float]],
    unit: str,
    log_scale: bool = False,
) -> None:
    width, height = 720, 300
    pad_left, pad_bottom, pad_top, pad_right = 70, 58, 40, 20
    plot_w = width - pad_left - pad_right
    plot_h = height - pad_top - pad_bottom

    values = [v for group in series.values() for v in group]
    peak = max(values) if values else 1.0
    peak = peak if peak > 0 else 1.0
    import math

    def scale(value: float) -> float:
        if log_scale:
            floor = max(peak / 1000.0, 1e-9)
            value = max(value, floor)
            return (math.log10(value) - math.log10(floor)) / (
                math.log10(peak) - math.log10(floor) + 1e-12
            )
        return value / peak

    n_groups = len(workloads)
    n_series = len(series)
    group_w = plot_w / max(n_groups, 1)
    bar_w = min(70.0, group_w * 0.68 / max(n_series, 1))
    colors = {"optimized": "#35e0c8", "baseline": "#f5b64b"}

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="ui-sans-serif, system-ui, sans-serif">',
        f'<rect width="{width}" height="{height}" fill="#0b1118"/>',
        f'<text x="{pad_left}" y="24" fill="#e6f1f7" font-size="15">{title}</text>',
        f'<text x="{pad_left}" y="40" fill="#6f8595" font-size="11">{unit}</text>',
    ]

    for gi, workload in enumerate(workloads):
        base_x = pad_left + gi * group_w + group_w / 2 - (bar_w * n_series) / 2
        for si, engine in enumerate(series):
            value = series[engine][gi]
            h = max(scale(value) * plot_h, 1.0)
            x = base_x + si * bar_w
            y = pad_top + plot_h - h
            parts.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_w - 6:.1f}" height="{h:.1f}" '
                f'fill="{colors.get(engine, "#8899aa")}" rx="2"/>'
            )
            label = f"{value:,.0f}" if value >= 100 else f"{value:,.2f}"
            parts.append(
                f'<text x="{x + (bar_w - 6) / 2:.1f}" y="{y - 5:.1f}" fill="#c7d6e0" '
                f'font-size="10" text-anchor="middle">{label}</text>'
            )
        parts.append(
            f'<text x="{pad_left + gi * group_w + group_w / 2:.1f}" '
            f'y="{pad_top + plot_h + 18}" fill="#8fa6b5" font-size="11" '
            f'text-anchor="middle">{workload}</text>'
        )

    parts.append(
        f'<line x1="{pad_left}" y1="{pad_top + plot_h}" x2="{width - pad_right}" '
        f'y2="{pad_top + plot_h}" stroke="#22303c"/>'
    )
    for si, engine in enumerate(series):
        x = pad_left + si * 210
        parts.append(
            f'<rect x="{x}" y="{height - 24}" width="10" height="10" '
            f'fill="{colors.get(engine, "#8899aa")}"/>'
            f'<text x="{x + 16}" y="{height - 15}" fill="#8fa6b5" font-size="11">'
            f"{LABELS.get(engine, engine)}</text>"
        )
    parts.append("</svg>")
    path.write_text("\n".join(parts))
    print(f"wrote {path.relative_to(ROOT)}")


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# Baseline vs optimized — measured comparison",
        "",
        f"Generated {report['generated_at']} by `scripts/run_baseline_comparison.sh`.",
        "",
        "Both engines execute the same pre-generated event vector: identical seed, workload, "
        "order distribution and capacity limits. Correctness is checked before any timing is "
        "reported.",
        "",
        f"Environment: {report['environment']['cpu']} · "
        f"{report['environment']['logical_cores']} logical cores · "
        f"{report['environment']['os']} · GCC {report['environment']['compiler']} · "
        "shared runner, no CPU pinning, no hardware counters.",
        "",
        "| workload | engine | msg/s | p50 | p99 | p99.9 | max | heap allocs | peak RSS |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in report["workloads"]:
        for engine in ENGINES:
            stats = row[engine]
            lines.append(
                f"| {row['workload']} | {LABELS[engine]} | "
                f"{stats['median_events_per_second'] / 1e6:,.2f} M | "
                f"{stats['median_p50_ns']:,.0f} ns | {stats['median_p99_ns']:,.0f} ns | "
                f"{stats['median_p99_9_ns']:,.0f} ns | {stats['median_max_ns']:,.0f} ns | "
                f"{stats['median_heap_allocations']:,.0f} | "
                f"{stats['median_peak_rss_kb'] / 1024:,.1f} MiB |"
            )

    lines += [
        "",
        "| workload | throughput ratio | p99 ratio | allocations avoided | state identical |",
        "| --- | ---: | ---: | ---: | :---: |",
    ]
    for row in report["workloads"]:
        opt, base = row["optimized"], row["baseline"]
        speed = (
            opt["median_events_per_second"] / base["median_events_per_second"]
            if base["median_events_per_second"]
            else float("inf")
        )
        p99_ratio = (
            base["median_p99_ns"] / opt["median_p99_ns"] if opt["median_p99_ns"] else float("inf")
        )
        lines.append(
            f"| {row['workload']} | {speed:,.2f}× | {p99_ratio:,.1f}× lower | "
            f"{base['median_heap_allocations'] - opt['median_heap_allocations']:,.0f} | "
            f"{'PASS' if row['correctness']['identical'] else 'FAIL'} |"
        )

    charts = ["throughput", "p99", "p999", "max", "allocations", "rss"]
    lines += ["", "## Charts", ""]
    for name in charts:
        lines.append(f"![{name}](charts/{name}.svg)")
        lines.append("")

    lines += [
        "## Methodology",
        "",
        "- Same `FeedGenerator` stream for both engines, generated once per process, outside "
        "the timed loop.",
        "- 20,000-event warm-up on a throwaway instance, then a fresh engine for the measured "
        "loop.",
        "- Each repeat runs in its own process, so peak RSS belongs to one implementation.",
        "- Per-event timing uses `steady_clock` and the same log-linear histogram in both "
        "engines; samples include clock-read overhead.",
        "- Heap allocations come from the global `operator new` override, reset immediately "
        "before the measured loop.",
        "- Correctness gate: final book CRC64, CRC64 at 25/50/100% of the stream, trade count, "
        "reject count, active orders, depth, and a CRC64 over the last 200 trades.",
        "",
        "Limitations: shared 2-vCPU runner without pinning; the baseline engine keeps a full "
        "trade vector while the optimized engine keeps a 65,536-entry ring, so the tail "
        "comparison covers only the most recent trades (the final state checksum covers the "
        "whole resting book).",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    grouped = load_runs()
    if not grouped:
        print("no cmp_*.json inputs found; run scripts/run_baseline_comparison.sh", file=sys.stderr)
        return 1

    workloads = []
    rows = []
    all_identical = True

    for workload, engines in sorted(grouped.items()):
        if not all(engine in engines for engine in ENGINES):
            print(f"{workload}: missing one engine's runs", file=sys.stderr)
            all_identical = False
            continue
        opt = sorted(engines["optimized"], key=lambda r: r["repeat"])
        base = sorted(engines["baseline"], key=lambda r: r["repeat"])
        correctness_result = correctness(opt, base)
        all_identical = all_identical and correctness_result["identical"]
        workloads.append(workload)
        rows.append(
            {
                "workload": workload,
                "events": int(opt[0]["events"]),
                "seed": int(opt[0]["seed"]),
                "optimized": summarise(opt),
                "baseline": summarise(base),
                "correctness": correctness_result,
                "raw": {"optimized": opt, "baseline": base},
            }
        )

    # Environment from the raw files (the summarised rows do not carry it).
    sample = next(iter(next(iter(grouped.values())).values()))[0]
    report = {
        "generated_at": sample.get("generated_at", ""),
        "environment": sample_env(sample),
        "workloads": rows,
        "all_identical": all_identical,
    }

    REPORTS.mkdir(exist_ok=True)
    CHARTS.mkdir(exist_ok=True)

    with open(REPORTS / "baseline_comparison.json", "w") as handle:
        json.dump(report, handle, indent=2)
    print("wrote reports/baseline_comparison.json")

    (REPORTS / "baseline_comparison.md").write_text(markdown(report))
    print("wrote reports/baseline_comparison.md")

    bar_chart(
        CHARTS / "throughput.svg",
        "End-to-end throughput (median of repeats)",
        workloads,
        {
            "optimized": [r["optimized"]["median_events_per_second"] / 1e6 for r in rows],
            "baseline": [r["baseline"]["median_events_per_second"] / 1e6 for r in rows],
        },
        "million messages per second — higher is better",
    )
    bar_chart(
        CHARTS / "p99.svg",
        "p99 latency (median of repeats)",
        workloads,
        {
            "optimized": [r["optimized"]["median_p99_ns"] for r in rows],
            "baseline": [r["baseline"]["median_p99_ns"] for r in rows],
        },
        "nanoseconds — lower is better (log scale)",
        log_scale=True,
    )
    bar_chart(
        CHARTS / "p999.svg",
        "p99.9 latency (median of repeats)",
        workloads,
        {
            "optimized": [r["optimized"]["median_p99_9_ns"] for r in rows],
            "baseline": [r["baseline"]["median_p99_9_ns"] for r in rows],
        },
        "nanoseconds — lower is better (log scale)",
        log_scale=True,
    )
    bar_chart(
        CHARTS / "max.svg",
        "Maximum latency (median of repeats)",
        workloads,
        {
            "optimized": [r["optimized"]["median_max_ns"] for r in rows],
            "baseline": [r["baseline"]["median_max_ns"] for r in rows],
        },
        "nanoseconds — lower is better (log scale)",
        log_scale=True,
    )
    bar_chart(
        CHARTS / "allocations.svg",
        "Heap allocations inside the measured region",
        workloads,
        {
            "optimized": [r["optimized"]["median_heap_allocations"] for r in rows],
            "baseline": [r["baseline"]["median_heap_allocations"] for r in rows],
        },
        "allocations per run — lower is better",
        log_scale=True,
    )
    bar_chart(
        CHARTS / "rss.svg",
        "Peak resident memory (per-process)",
        workloads,
        {
            "optimized": [r["optimized"]["median_peak_rss_kb"] / 1024 for r in rows],
            "baseline": [r["baseline"]["median_peak_rss_kb"] / 1024 for r in rows],
        },
        "MiB — lower is better",
    )

    print(json.dumps(
        {
            "workloads": workloads,
            "correctness": "PASS" if all_identical else "FAIL",
        },
        indent=2,
    ))
    return 0 if all_identical else 1


def sample_env(run: dict[str, Any]) -> dict[str, Any]:
    env = run.get("environment", {})
    return {
        "cpu": env.get("cpu", "unknown"),
        "logical_cores": env.get("logical_cores", 0),
        "os": env.get("os", "unknown"),
        "compiler": env.get("compiler", "unknown"),
        "notes": "shared runner, no CPU pinning, clock std::chrono::steady_clock",
    }


if __name__ == "__main__":
    sys.exit(main())
