// UltraLOB — baseline vs optimized A/B harness.
//
// Both engines execute the *same pre-generated event vector* (identical seed,
// workload and order distribution) and are compared on book checksum, trade
// count, reject count and trade tail. If they disagree the report generator
// fails the comparison.
//
// Memory is measured per process: scripts/run_baseline_comparison.sh invokes
// this binary once per (engine, repeat) so peak RSS belongs to a single
// implementation instead of a mixed heap.
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "allocation_tracker.hpp"
#include "baseline_engine.hpp"

#include "ultralob/config.hpp"
#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/latency_histogram.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/order_index.hpp"

namespace {

using namespace ultralob;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kTailTrades = 200;
constexpr std::size_t kWarmupEvents = 20'000;

std::string read_first_line_containing(const char* path, const char* needle) {
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find(needle) != std::string::npos) return line;
  }
  return {};
}

std::string cpu_model() {
  const std::string line = read_first_line_containing("/proc/cpuinfo", "model name");
  const auto colon = line.find(':');
  if (colon == std::string::npos) return "unknown";
  const std::string value = line.substr(colon + 1);
  const auto start = value.find_first_not_of(' ');
  return start == std::string::npos ? "unknown" : value.substr(start);
}

std::string os_name() {
  const std::string line = read_first_line_containing("/etc/os-release", "PRETTY_NAME=");
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

bool parse_workload(const std::string& name, Workload* out) {
  if (name == "balanced") {
    *out = Workload::Balanced;
    return true;
  }
  if (name == "cancel_heavy") {
    *out = Workload::CancelHeavy;
    return true;
  }
  if (name == "burst") {
    *out = Workload::Burst;
    return true;
  }
  return false;
}

std::uint64_t tail_crc(const Trade* trades, std::size_t count) {
  std::uint64_t crc = kCrc64Init;
  for (std::size_t i = 0; i < count; ++i) {
    crc = crc64_update_u64(crc, trades[i].id);
    crc = crc64_update_u64(crc, trades[i].maker);
    crc = crc64_update_u64(crc, trades[i].taker);
    crc = crc64_update_u64(crc, static_cast<std::uint64_t>(trades[i].price));
    crc = crc64_update_u64(crc, trades[i].qty);
    crc = crc64_update_u64(crc, trades[i].seq);
    crc = crc64_update_u64(crc, static_cast<std::uint64_t>(trades[i].taker_side));
  }
  return crc;
}

struct State {
  std::uint64_t trades = 0;
  std::uint64_t rejected = 0;
  std::uint64_t active = 0;
  std::uint64_t depth_bid = 0;
  std::uint64_t depth_ask = 0;
  std::uint64_t tail_crc = 0;
};

// Common interface so both implementations go through identical timing,
// warm-up, allocation accounting and state capture.
struct Harness {
  virtual ~Harness() = default;
  virtual void apply(const Event& event) = 0;
  virtual std::uint64_t crc() const = 0;
  virtual State state() const = 0;
};

class OptimizedHarness final : public Harness {
 public:
  OptimizedHarness() {
    EventProcessor::Config config{};
    config.gap_policy = GapPolicy::Buffer;
    config.duplicate_policy = DuplicatePolicy::Reject;
    config.start_seq = 1;
    processor_ = std::make_unique<EventProcessor>(*engine_, config);
  }

  void apply(const Event& event) override {
    const ProcessOutcome outcome = processor_->process(event);
    if (outcome.status == ProcessStatus::Rejected) ++rejected_;
  }

  std::uint64_t crc() const override { return engine_->crc64(); }

  State state() const override {
    State state{};
    state.trades = engine_->trade_count();
    state.rejected = rejected_;
    state.active = engine_->active_orders();
    state.depth_bid = engine_->depth(Side::Buy);
    state.depth_ask = engine_->depth(Side::Sell);
    Trade tail[kTailTrades]{};
    const std::size_t count = engine_->recent_trades(tail, kTailTrades);
    state.tail_crc = tail_crc(tail, count);
    return state;
  }

 private:
  std::unique_ptr<MatchingEngine> engine_ = std::make_unique<MatchingEngine>();
  std::unique_ptr<EventProcessor> processor_;
  std::uint64_t rejected_ = 0;
};

// Same ordering rules as EventProcessor, for the baseline engine (the processor
// is typed to MatchingEngine, so the harness mirrors its sequence/duplicate
// checks explicitly). This slightly favours the baseline: it does less work per
// event than the shipped processor.
class BaselineHarness final : public Harness {
 public:
  void apply(const Event& event) override {
    if (event.seq == 0) return;
    const std::size_t slot = static_cast<std::size_t>(OrderIndex::hash(event.id)) &
                             (Limits::kRecentEventIds - 1);
    if (event.id != 0 && seen_[slot] == event.id) return;  // duplicate: not applied
    if (event.seq != last_applied_ + 1) return;            // gap / out of order: not applied

    SubmitResult result{};
    switch (event.type) {
      case EventType::NewOrder:
        result = event.order_type == OrderType::Market
                     ? engine_.submit_market(event.side, event.qty, event.seq, event.order_id)
                     : engine_.submit_limit(event.side, event.price, event.qty, event.seq,
                                            event.order_id);
        break;
      case EventType::CancelOrder:
        result = engine_.cancel(event.order_id, event.seq);
        break;
      case EventType::ModifyOrder:
        result = engine_.modify(event.order_id, event.price, event.qty, event.seq);
        break;
      default:
        result.accepted = true;
        break;
    }

    last_applied_ = event.seq;
    if (event.id != 0) seen_[slot] = event.id;
    if (!result.accepted) ++rejected_;
  }

  std::uint64_t crc() const override { return engine_.crc64(); }

  State state() const override {
    State state{};
    state.trades = engine_.trade_count();
    state.rejected = rejected_;
    state.active = engine_.active_orders();
    state.depth_bid = engine_.depth(Side::Buy);
    state.depth_ask = engine_.depth(Side::Sell);
    Trade tail[kTailTrades]{};
    const std::size_t count = engine_.recent_trades(tail, kTailTrades);
    state.tail_crc = tail_crc(tail, count);
    return state;
  }

 private:
  baseline::Engine engine_{};
  std::array<std::uint64_t, Limits::kRecentEventIds> seen_{};
  Seq last_applied_ = 0;
  std::uint64_t rejected_ = 0;
};

struct Run {
  std::string engine;
  std::string workload;
  std::uint64_t events = 0;
  std::uint64_t seed = 0;
  int repeat = 0;
  double elapsed_ms = 0.0;
  double events_per_second = 0.0;
  double matching_events_per_second = 0.0;
  std::uint64_t p50 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0, mean = 0;
  std::uint64_t allocations = 0, allocated_bytes = 0, peak_rss = 0;
  std::uint64_t trades = 0, rejected = 0, active = 0;
  std::uint64_t depth_bid = 0, depth_ask = 0;
  std::uint64_t crc = 0, trade_tail_crc = 0;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> checkpoints;
};

Run measure(const std::string& engine_name, const std::vector<Event>& stream,
            Workload workload, std::uint64_t seed, int repeat,
            const std::function<std::unique_ptr<Harness>()>& factory) {
  Run run{};
  run.engine = engine_name;
  run.workload = workload_name(workload);
  run.events = static_cast<std::uint64_t>(stream.size());
  run.seed = seed;
  run.repeat = repeat;

  // Warm-up on a throwaway instance: page faults on the level tables / pools,
  // branch predictor and allocator arenas should not land in the measured loop.
  {
    const std::unique_ptr<Harness> warm = factory();
    const std::size_t warm_events = std::min<std::size_t>(kWarmupEvents, stream.size() / 10);
    for (std::size_t i = 0; i < warm_events; ++i) warm->apply(stream[i]);
  }

  const std::unique_ptr<Harness> harness = factory();
  LatencyHistogram histogram;
  std::uint64_t matching_ns = 0;
  const std::uint64_t check_at[3] = {stream.size() / 4, stream.size() / 2, stream.size()};
  std::size_t next_checkpoint = 0;
  // Reserve so checkpoint bookkeeping does not appear in the allocation counts
  // (it is harness work, not engine work).
  run.checkpoints.reserve(3);

  ultralob_bench::reset_allocations();
  const auto start_all = Clock::now();
  for (std::size_t i = 0; i < stream.size(); ++i) {
    const auto started = Clock::now();
    harness->apply(stream[i]);
    const auto finished = Clock::now();
    const std::uint64_t ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    histogram.record(ns);
    matching_ns += ns;

    const std::uint64_t done = static_cast<std::uint64_t>(i + 1);
    if (next_checkpoint < 3 && done == check_at[next_checkpoint]) {
      run.checkpoints.emplace_back(done, harness->crc());
      ++next_checkpoint;
    }
  }
  const auto end_all = Clock::now();
  const std::uint64_t wall_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end_all - start_all).count());
  const ultralob_bench::AllocationSnapshot alloc = ultralob_bench::allocations();
  const State state = harness->state();

  run.elapsed_ms = static_cast<double>(wall_ns) / 1e6;
  run.events_per_second =
      wall_ns == 0 ? 0.0 : static_cast<double>(run.events) * 1e9 / static_cast<double>(wall_ns);
  run.matching_events_per_second =
      matching_ns == 0 ? 0.0
                       : static_cast<double>(run.events) * 1e9 / static_cast<double>(matching_ns);
  run.p50 = histogram.percentile(50.0);
  run.p99 = histogram.percentile(99.0);
  run.p999 = histogram.percentile(99.9);
  run.p9999 = histogram.percentile(99.99);
  run.max = histogram.max();
  run.mean = histogram.mean();
  run.allocations = alloc.allocations;
  run.allocated_bytes = alloc.bytes;
  run.peak_rss = peak_rss_kb();
  run.trades = state.trades;
  run.rejected = state.rejected;
  run.active = state.active;
  run.depth_bid = state.depth_bid;
  run.depth_ask = state.depth_ask;
  run.trade_tail_crc = state.tail_crc;
  run.crc = harness->crc();
  return run;
}

void print_json(const std::string& engine_name, Workload workload, std::uint64_t seed,
                const std::vector<Run>& runs, std::FILE* out) {
  std::fprintf(out, "{\n");
  std::fprintf(out, "  \"generated_at\": \"%s\",\n", isoTimestamp().c_str());
  std::fprintf(out, "  \"harness\": \"benchmarks/compare_benchmarks.cpp\",\n");
  std::fprintf(out, "  \"engine\": \"%s\",\n", engine_name.c_str());
  std::fprintf(out, "  \"workload\": \"%s\",\n", workload_name(workload));
  std::fprintf(out, "  \"seed\": %" PRIu64 ",\n", seed);
  std::fprintf(out, "  \"environment\": {\n");
  std::fprintf(out, "    \"cpu\": \"%s\",\n", cpu_model().c_str());
  std::fprintf(out, "    \"logical_cores\": %ld,\n", sysconf(_SC_NPROCESSORS_ONLN));
  std::fprintf(out, "    \"os\": \"%s\",\n", os_name().c_str());
  std::fprintf(out, "    \"compiler\": \"%s\",\n", __VERSION__);
  std::fprintf(out, "    \"clock\": \"std::chrono::steady_clock (CLOCK_MONOTONIC)\",\n");
  std::fprintf(out, "    \"cpu_affinity\": \"not pinned (shared cloud runner)\",\n");
  std::fprintf(out,
               "    \"warmup_events\": %zu,\n", kWarmupEvents);
  std::fprintf(out,
               "    \"peak_rss_scope\": \"whole process: includes the pre-generated feed and "
               "the implementation's live heap\"\n");
  std::fprintf(out, "  },\n");
  std::fprintf(out, "  \"runs\": [\n");
  for (std::size_t r = 0; r < runs.size(); ++r) {
    const Run& run = runs[r];
    std::fprintf(out,
                 "    {\"workload\": \"%s\", \"events\": %" PRIu64 ", \"seed\": %" PRIu64
                 ", \"repeat\": %d, \"elapsed_ms\": %.2f, \"events_per_second\": %.0f, "
                 "\"matching_events_per_second\": %.0f, "
                 "\"latency_ns\": {\"p50\": %" PRIu64 ", \"p99\": %" PRIu64
                 ", \"p99_9\": %" PRIu64 ", \"p99_99\": %" PRIu64 ", \"max\": %" PRIu64
                 ", \"mean\": %" PRIu64 "}, \"heap_allocations\": %" PRIu64
                 ", \"allocated_bytes\": %" PRIu64 ", \"peak_rss_kb\": %" PRIu64
                 ", \"trades\": %" PRIu64 ", \"rejected\": %" PRIu64
                 ", \"active_orders\": %" PRIu64 ", \"depth_bid\": %" PRIu64
                 ", \"depth_ask\": %" PRIu64 ", \"crc64\": \"%" PRIu64
                 "\", \"trade_tail_crc\": \"%" PRIu64 "\", \"checkpoints\": [",
                 run.workload.c_str(), run.events, run.seed, run.repeat, run.elapsed_ms,
                 run.events_per_second, run.matching_events_per_second, run.p50, run.p99,
                 run.p999, run.p9999, run.max, run.mean, run.allocations, run.allocated_bytes,
                 run.peak_rss, run.trades, run.rejected, run.active, run.depth_bid,
                 run.depth_ask, run.crc, run.trade_tail_crc);
    for (std::size_t i = 0; i < run.checkpoints.size(); ++i) {
      std::fprintf(out, "%s{\"events\": %" PRIu64 ", \"crc64\": \"%" PRIu64 "\"}",
                   i == 0 ? "" : ", ", run.checkpoints[i].first, run.checkpoints[i].second);
    }
    std::fprintf(out, "]}%s\n", r + 1 == runs.size() ? "" : ",");
  }
  std::fprintf(out, "  ]\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string engine_name = "optimized";
  std::string workload_name_arg = "balanced";
  std::uint64_t events = 1'000'000;
  std::uint64_t seed = 2024;
  int repeats = 3;
  std::string json_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (arg == "--engine") {
      engine_name = value();
    } else if (arg == "--workload") {
      workload_name_arg = value();
    } else if (arg == "--events") {
      events = std::strtoull(value(), nullptr, 10);
    } else if (arg == "--seed") {
      seed = std::strtoull(value(), nullptr, 10);
    } else if (arg == "--repeats") {
      repeats = std::atoi(value());
    } else if (arg == "--json-out") {
      json_path = value();
    } else if (arg == "--help") {
      std::printf(
          "usage: ultralob_compare --engine optimized|baseline "
          "[--workload balanced|cancel_heavy|burst] [--events N] [--seed S] "
          "[--repeats R] [--json-out path]\n");
      return 0;
    }
  }

  Workload workload{};
  if (!parse_workload(workload_name_arg, &workload)) {
    std::fprintf(stderr, "unknown workload '%s' (expected balanced, cancel_heavy or burst)\n",
                 workload_name_arg.c_str());
    return 2;
  }
  if (engine_name != "optimized" && engine_name != "baseline") {
    std::fprintf(stderr, "unknown engine '%s' (expected optimized or baseline)\n",
                 engine_name.c_str());
    return 2;
  }
  if (events == 0) {
    std::fprintf(stderr, "--events must be greater than zero\n");
    return 2;
  }
  if (repeats < 1) repeats = 1;

  std::vector<Event> stream;
  stream.reserve(static_cast<std::size_t>(events));
  {
    FeedGenerator generator(workload_config(workload, seed));
    for (std::uint64_t i = 0; i < events; ++i) stream.push_back(generator.next());
  }

  const std::function<std::unique_ptr<Harness>()> factory =
      engine_name == "optimized"
          ? std::function<std::unique_ptr<Harness>()>([&] {
              return std::unique_ptr<Harness>(new OptimizedHarness());
            })
          : std::function<std::unique_ptr<Harness>()>([&] {
              return std::unique_ptr<Harness>(new BaselineHarness());
            });

  std::vector<Run> runs;
  runs.reserve(static_cast<std::size_t>(repeats));
  for (int repeat = 1; repeat <= repeats; ++repeat) {
    runs.push_back(measure(engine_name, stream, workload, seed, repeat, factory));
    const Run& run = runs.back();
    std::fprintf(stderr,
                 "%s %-12s events=%" PRIu64 " repeat=%d  %.2f M msg/s  p50=%" PRIu64
                 "ns p99=%" PRIu64 "ns allocs=%" PRIu64 " rss=%" PRIu64 "kB\n",
                 run.engine.c_str(), run.workload.c_str(), run.events, run.repeat,
                 run.events_per_second / 1e6, run.p50, run.p99, run.allocations, run.peak_rss);
  }

  std::FILE* out = json_path.empty() ? stdout : std::fopen(json_path.c_str(), "w");
  if (out == nullptr) {
    std::fprintf(stderr, "could not open %s for writing\n", json_path.c_str());
    return 1;
  }
  print_json(engine_name, workload, seed, runs, out);
  if (out != stdout) std::fclose(out);
  return 0;
}
