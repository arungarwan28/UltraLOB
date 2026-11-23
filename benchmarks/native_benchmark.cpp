// UltraLOB — native benchmark harness.
//
// Measures the same engine the browser runs, on Linux, with the environment
// recorded next to every number. Feed generation is excluded from the
// per-event latency sample (it happens outside the timed region) but included
// in the end-to-end throughput figure. Both are labelled.
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "allocation_tracker.hpp"

#include "ultralob/config.hpp"
#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/latency_histogram.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/memory_pool.hpp"
#include "ultralob/order_index.hpp"

namespace {

using namespace ultralob;
using Clock = std::chrono::steady_clock;

// Recorded in the report so a run can be reproduced with the flags that made it.
#ifndef ULTRALOB_BUILD_FLAGS
#define ULTRALOB_BUILD_FLAGS "(build flags not captured by this build system)"
#endif

std::string read_first_line_containing(const char* path, const char* needle) {
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find(needle) != std::string::npos) return line;
  }
  return {};
}

std::string cpu_model() {
  std::string line = read_first_line_containing("/proc/cpuinfo", "model name");
  const auto colon = line.find(':');
  if (colon == std::string::npos) return "unknown";
  std::string value = line.substr(colon + 1);
  const auto start = value.find_first_not_of(' ');
  return start == std::string::npos ? "unknown" : value.substr(start);
}

std::string os_name() {
  std::string line = read_first_line_containing("/etc/os-release", "PRETTY_NAME=");
  if (line.empty()) return "unknown";
  const auto first_quote = line.find('"');
  const auto last_quote = line.rfind('"');
  if (first_quote == std::string::npos || last_quote <= first_quote) return line;
  return line.substr(first_quote + 1, last_quote - first_quote - 1);
}

std::string isoTimestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

std::uint64_t peak_rss_kb() {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  return static_cast<std::uint64_t>(usage.ru_maxrss);
}

Workload parse_workload(const std::string& name, bool* ok) {
  if (name == "balanced") return Workload::Balanced;
  if (name == "cancel_heavy") return Workload::CancelHeavy;
  if (name == "burst") return Workload::Burst;
  *ok = false;
  return Workload::Balanced;
}

struct RunResult {
  std::string workload;
  std::uint64_t events = 0;
  std::uint64_t seed = 0;
  int repeat = 0;
  double elapsed_ms = 0.0;
  double events_per_second = 0.0;
  double matching_events_per_second = 0.0;
  std::uint64_t trades = 0;
  std::uint64_t rejected = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t gaps = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
  std::uint64_t p9999_ns = 0;
  std::uint64_t max_ns = 0;
  std::uint64_t mean_ns = 0;
  std::uint64_t heap_allocations = 0;
  std::uint64_t allocated_bytes = 0;
  std::uint64_t peak_rss_kb = 0;
  std::uint64_t active_orders = 0;
  std::uint64_t crc64 = 0;
};

RunResult run_once(Workload workload, std::uint64_t seed, std::uint64_t events,
                   int repeat) {
  RunResult result{};
  result.workload = workload_name(workload);
  result.events = events;
  result.seed = seed;
  result.repeat = repeat;

  auto engine = std::make_unique<MatchingEngine>();

  EventProcessor::Config processor_config{};
  processor_config.gap_policy = GapPolicy::Buffer;
  processor_config.duplicate_policy = DuplicatePolicy::Reject;
  processor_config.start_seq = 1;
  EventProcessor processor(*engine, processor_config);
  FeedGenerator generator(workload_config(workload, seed));

  // Warm-up outside the measured region: page in the pool, populate the index.
  for (int i = 0; i < 2000; ++i) processor.process(generator.next());
  engine->reset();
  processor.reset(processor_config);
  generator = FeedGenerator(workload_config(workload, seed));

  LatencyHistogram histogram;
  ultralob_bench::reset_allocations();

  const auto wall_start = Clock::now();
  std::uint64_t matching_ns = 0;
  for (std::uint64_t i = 0; i < events; ++i) {
    const Event event = generator.next();
    const auto started = Clock::now();
    const ProcessOutcome outcome = processor.process(event);
    const auto finished = Clock::now();
    const auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started);
    histogram.record(static_cast<std::uint64_t>(duration.count()));
    matching_ns += static_cast<std::uint64_t>(duration.count());
    if (outcome.status == ProcessStatus::Rejected) ++result.rejected;
  }
  const auto wall_end = Clock::now();

  const auto wall_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
  const auto allocation_snapshot = ultralob_bench::allocations();

  result.elapsed_ms = static_cast<double>(wall_ns) / 1'000'000.0;
  result.events_per_second =
      static_cast<double>(events) / (static_cast<double>(wall_ns) / 1'000'000'000.0);
  result.matching_events_per_second =
      matching_ns == 0 ? 0.0
                       : static_cast<double>(events) /
                             (static_cast<double>(matching_ns) / 1'000'000'000.0);
  result.trades = engine->trade_count();
  result.duplicates = processor.stats().duplicates;
  result.gaps = processor.stats().gaps;
  result.p50_ns = histogram.percentile(50.0);
  result.p99_ns = histogram.percentile(99.0);
  result.p999_ns = histogram.percentile(99.9);
  result.p9999_ns = histogram.percentile(99.99);
  result.max_ns = histogram.max();
  result.mean_ns = histogram.mean();
  result.heap_allocations = allocation_snapshot.allocations;
  result.allocated_bytes = allocation_snapshot.bytes;
  result.peak_rss_kb = peak_rss_kb();
  result.active_orders = engine->active_orders();
  result.crc64 = engine->crc64();
  return result;
}

struct ComponentResult {
  std::string name;
  double ns_per_op = 0.0;
  double ops = 0.0;
};

// Keeps the optimiser from deleting a loop whose result is unused.
inline void keep_alive(std::uint64_t& value) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : "+r"(value));
#else
  volatile std::uint64_t sink = value;
  (void)sink;
#endif
}

std::vector<ComponentResult> run_components() {
  std::vector<ComponentResult> results;

  {
    constexpr int kOps = 2'000'000;
    ObjectPool<Order, 4096> pool;
    std::uint64_t sink = 0;
    const auto start = Clock::now();
    for (int i = 0; i < kOps; ++i) {
      const std::uint32_t slot = pool.acquire();
      sink += slot;
      pool.release(slot);
    }
    keep_alive(sink);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - start)
                             .count();
    results.push_back({"pool_acquire_release", static_cast<double>(elapsed) / kOps,
                       static_cast<double>(kOps)});
  }

  {
    constexpr int kOps = 1'000'000;
    OrderIndex index;
    std::uint64_t sink = 0;
    const auto start = Clock::now();
    for (int i = 0; i < kOps; ++i) index.insert(static_cast<OrderId>(i + 1), i % 4096);
    for (int i = 0; i < kOps; ++i) sink += index.find(static_cast<OrderId>(i + 1));
    for (int i = 0; i < kOps; ++i) index.erase(static_cast<OrderId>(i + 1));
    keep_alive(sink);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - start)
                             .count();
    results.push_back({"order_index_insert_find_erase", static_cast<double>(elapsed) / kOps,
                       static_cast<double>(kOps)});
  }

  {
    auto engine = std::make_unique<MatchingEngine>();
    constexpr int kOps = 200'000;
    const auto start = Clock::now();
    for (int i = 0; i < kOps; ++i) {
      // Alternate prices so each submit performs a level lookup and a trade.
      engine->submit_limit(Side::Buy, 9000 + (i % 400), 1, static_cast<Seq>(i + 1));
      engine->submit_limit(Side::Sell, 9400 + (i % 400), 1, static_cast<Seq>(i + 1));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - start)
                             .count();
    results.push_back({"submit_limit_round_trip", static_cast<double>(elapsed) / (kOps * 2),
                       static_cast<double>(kOps * 2)});
  }

  return results;
}

void print_json(const std::vector<RunResult>& runs,
                const std::vector<ComponentResult>& components, std::FILE* out) {
  std::fprintf(out, "{\n");
  std::fprintf(out, "  \"generated_at\": \"%s\",\n", isoTimestamp().c_str());
  std::fprintf(out, "  \"harness\": \"benchmarks/native_benchmark.cpp\",\n");
  std::fprintf(out, "  \"environment\": {\n");
  std::fprintf(out, "    \"cpu\": \"%s\",\n", cpu_model().c_str());
  std::fprintf(out, "    \"logical_cores\": %ld,\n", sysconf(_SC_NPROCESSORS_ONLN));
  std::fprintf(out, "    \"os\": \"%s\",\n", os_name().c_str());
  std::fprintf(out, "    \"compiler\": \"%s\",\n", __VERSION__);
  std::fprintf(out, "    \"build_flags\": \"%s\",\n", ULTRALOB_BUILD_FLAGS);
  std::fprintf(out,
               "    \"clock\": \"std::chrono::steady_clock (CLOCK_MONOTONIC), per-event "
               "samples include timer overhead\",\n");
  std::fprintf(out, "    \"cpu_affinity\": \"not pinned (shared cloud runner)\"\n");
  std::fprintf(out, "  },\n");
  std::fprintf(out,
               "  \"engine_limits\": {\"max_orders\": %zu, \"max_levels_per_side\": %zu, "
               "\"max_trade_log\": %zu},\n",
               Limits::kMaxOrders, Limits::kMaxLevelsPerSide, Limits::kMaxTradeLog);

  std::fprintf(out, "  \"runs\": [\n");
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const RunResult& run = runs[i];
    std::fprintf(out,
                 "    {\"workload\": \"%s\", \"events\": %" PRIu64 ", \"seed\": %" PRIu64
                 ", \"repeat\": %d, \"elapsed_ms\": %.2f, \"events_per_second\": %.0f, "
                 "\"matching_events_per_second\": %.0f, \"trades\": %" PRIu64
                 ", \"rejected\": %" PRIu64 ", \"duplicates\": %" PRIu64
                 ", \"gaps\": %" PRIu64 ", \"latency_ns\": {\"p50\": %" PRIu64
                 ", \"p99\": %" PRIu64 ", \"p99_9\": %" PRIu64 ", \"p99_99\": %" PRIu64
                 ", \"max\": %" PRIu64 ", \"mean\": %" PRIu64 "}, "
                 "\"heap_allocations\": %" PRIu64 ", \"allocated_bytes\": %" PRIu64
                 ", \"peak_rss_kb\": %" PRIu64 ", \"active_orders\": %" PRIu64
                 ", \"crc64\": \"%" PRIu64 "\"}%s\n",
                 run.workload.c_str(), run.events, run.seed, run.repeat, run.elapsed_ms,
                 run.events_per_second, run.matching_events_per_second, run.trades,
                 run.rejected, run.duplicates, run.gaps, run.p50_ns, run.p99_ns,
                 run.p999_ns, run.p9999_ns, run.max_ns, run.mean_ns, run.heap_allocations,
                 run.allocated_bytes, run.peak_rss_kb, run.active_orders, run.crc64,
                 i + 1 == runs.size() ? "" : ",");
  }
  std::fprintf(out, "  ],\n");

  std::fprintf(out, "  \"components\": [\n");
  for (std::size_t i = 0; i < components.size(); ++i) {
    std::fprintf(out,
                 "    {\"name\": \"%s\", \"ops\": %.0f, \"ns_per_op\": %.2f}%s\n",
                 components[i].name.c_str(), components[i].ops, components[i].ns_per_op,
                 i + 1 == components.size() ? "" : ",");
  }
  std::fprintf(out, "  ]\n}\n");
}

void print_table(const std::vector<RunResult>& runs,
                 const std::vector<ComponentResult>& components) {
  std::printf("\n%-14s %9s %10s %12s %9s %9s %9s %9s %8s\n", "workload", "events",
              "ms", "M msg/s", "p50 ns", "p99 ns", "p99.9 ns", "max ns", "allocs");
  for (const RunResult& run : runs) {
    std::printf("%-14s %9" PRIu64 " %10.1f %12.2f %9" PRIu64 " %9" PRIu64 " %9" PRIu64
                " %9" PRIu64 " %8" PRIu64 "\n",
                run.workload.c_str(), run.events, run.elapsed_ms,
                run.events_per_second / 1'000'000.0, run.p50_ns, run.p99_ns, run.p999_ns,
                run.max_ns, run.heap_allocations);
  }
  std::printf("\n%-34s %14s %12s\n", "component", "ops", "ns/op");
  for (const ComponentResult& component : components) {
    std::printf("%-34s %14.0f %12.2f\n", component.name.c_str(), component.ops,
                component.ns_per_op);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t events = 1'000'000;
  std::uint64_t seed = 2024;
  std::string workload_name_arg = "balanced";
  int repeats = 3;
  std::string json_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (arg == "--events") {
      events = std::strtoull(next(), nullptr, 10);
    } else if (arg == "--seed") {
      seed = std::strtoull(next(), nullptr, 10);
    } else if (arg == "--workload") {
      workload_name_arg = next();
    } else if (arg == "--repeats") {
      repeats = std::atoi(next());
    } else if (arg == "--json-out") {
      json_path = next();
    } else if (arg == "--help") {
      std::printf(
          "usage: ultralob_bench [--events N] [--seed S] [--workload balanced|"
          "cancel_heavy|burst] [--repeats R] [--json-out path]\n");
      return 0;
    }
  }
  if (repeats < 1) repeats = 1;

  bool workload_ok = true;
  const Workload workload = parse_workload(workload_name_arg, &workload_ok);
  if (!workload_ok) {
    std::fprintf(stderr, "unknown workload '%s' (expected balanced, cancel_heavy or burst)\n",
                 workload_name_arg.c_str());
    return 2;
  }
  std::vector<RunResult> runs;
  runs.reserve(static_cast<std::size_t>(repeats));
  for (int repeat = 1; repeat <= repeats; ++repeat) {
    runs.push_back(run_once(workload, seed, events, repeat));
  }
  const std::vector<ComponentResult> components = run_components();

  print_table(runs, components);

  if (!json_path.empty()) {
    std::FILE* out = std::fopen(json_path.c_str(), "w");
    if (out != nullptr) {
      print_json(runs, components, out);
      std::fclose(out);
      std::printf("\nwrote %s\n", json_path.c_str());
    } else {
      std::fprintf(stderr, "could not open %s for writing\n", json_path.c_str());
      return 1;
    }
  } else {
    print_json(runs, components, stdout);
  }
  return 0;
}
