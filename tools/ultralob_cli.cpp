// UltraLOB — native CLI.
//
// Three jobs:
//   replay  : run a deterministic stream and emit checkpoints (used by the
//             Python parity test and to produce evidence artefacts)
//   verify  : two independent replays of the same stream must agree
//   chaos   : print the reliability scenario reports as JSON
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ultralob/chaos.hpp"
#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/snapshot.hpp"

namespace {

using namespace ultralob;

constexpr std::size_t kCheckpointLevels = 5;

struct Options {
  std::uint64_t events = 50000;
  std::uint64_t seed = 2024;
  std::string workload = "balanced";
  std::uint64_t checkpoint = 10000;
  std::string state_out;
  std::string events_out;
};

Workload parse_workload(const std::string& name) {
  if (name == "cancel_heavy") return Workload::CancelHeavy;
  if (name == "burst") return Workload::Burst;
  return Workload::Balanced;
}

struct Checkpoint {
  Seq seq = 0;
  std::uint64_t crc64 = 0;
  std::uint64_t trades = 0;
  std::uint64_t active_orders = 0;
  std::vector<std::pair<Price, Qty>> bids;
  std::vector<std::pair<Price, Qty>> asks;
};

struct ReplayResult {
  std::vector<Checkpoint> checkpoints;
  Snapshot final_state{};
  EventProcessor::Stats stats{};
};

ReplayResult run_stream(const Options& options, std::FILE* events_dump) {
  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor::Config config{};
  config.gap_policy = GapPolicy::Buffer;
  config.duplicate_policy = DuplicatePolicy::Reject;
  config.start_seq = 1;

  EventProcessor processor(*engine, config);
  FeedGenerator generator(workload_config(parse_workload(options.workload), options.seed));

  ReplayResult result{};
  for (std::uint64_t i = 0; i < options.events; ++i) {
    const Event event = generator.next();
    if (events_dump != nullptr) {
      std::fprintf(events_dump,
                   "{\"seq\": %" PRIu64 ", \"type\": \"%s\", \"side\": \"%s\", "
                   "\"order_type\": \"%s\", \"price\": %" PRId64 ", \"qty\": %" PRIu64
                   ", \"order_id\": %" PRIu64 "}\n",
                   event.seq, event_type_name(event.type), side_name(event.side),
                   event.order_type == OrderType::Market ? "market" : "limit",
                   event.price, event.qty, event.order_id);
    }
    processor.process(event);

    const bool at_boundary = options.checkpoint > 0 && ((i + 1) % options.checkpoint == 0);
    if (at_boundary || i + 1 == options.events) {
      Checkpoint checkpoint{};
      checkpoint.seq = processor.last_applied_seq();
      checkpoint.crc64 = engine->crc64();
      checkpoint.trades = engine->trade_count();
      checkpoint.active_orders = engine->active_orders();
      for (std::size_t level = 0; level < kCheckpointLevels; ++level) {
        if (const PriceLevel* bid = engine->level(Side::Buy, level)) {
          checkpoint.bids.emplace_back(bid->price, bid->total_qty);
        }
        if (const PriceLevel* ask = engine->level(Side::Sell, level)) {
          checkpoint.asks.emplace_back(ask->price, ask->total_qty);
        }
      }
      result.checkpoints.push_back(std::move(checkpoint));
    }
  }

  result.final_state = take_snapshot(*engine, options.events, processor.last_applied_seq());
  result.stats = processor.stats();
  return result;
}

void write_state_json(const ReplayResult& result, const Options& options, std::FILE* out) {
  std::fprintf(out, "{\n");
  std::fprintf(out, "  \"engine\": \"ultralob\",\n");
  std::fprintf(out, "  \"events\": %" PRIu64 ",\n", options.events);
  std::fprintf(out, "  \"seed\": %" PRIu64 ",\n", options.seed);
  std::fprintf(out, "  \"workload\": \"%s\",\n", options.workload.c_str());
  std::fprintf(out, "  \"checkpoint_interval\": %" PRIu64 ",\n", options.checkpoint);
  std::fprintf(out,
               "  \"final\": {\"seq\": %" PRIu64 ", \"crc64\": \"%" PRIu64 "\", "
               "\"trades\": %" PRIu64 ", \"bid_levels\": %zu, \"ask_levels\": %zu, "
               "\"bid_total\": %" PRIu64 ", \"ask_total\": %" PRIu64 "},\n",
               result.final_state.seq, result.final_state.crc64, result.final_state.trades,
               result.final_state.bid_levels, result.final_state.ask_levels,
               result.final_state.bid_total, result.final_state.ask_total);
  std::fprintf(out,
               "  \"processor\": {\"applied\": %" PRIu64 ", \"engine_rejected\": %" PRIu64
               ", \"duplicates\": %" PRIu64 ", \"gaps\": %" PRIu64
               ", \"out_of_order\": %" PRIu64 ", \"buffered\": %" PRIu64 "},\n",
               result.stats.applied, result.stats.engine_rejected, result.stats.duplicates,
               result.stats.gaps, result.stats.out_of_order, result.stats.buffered);
  std::fprintf(out, "  \"checkpoints\": [\n");
  for (std::size_t i = 0; i < result.checkpoints.size(); ++i) {
    const Checkpoint& checkpoint = result.checkpoints[i];
    std::fprintf(out,
                 "    {\"seq\": %" PRIu64 ", \"crc64\": \"%" PRIu64 "\", \"trades\": %" PRIu64
                 ", \"active_orders\": %" PRIu64 ", \"bids\": [",
                 checkpoint.seq, checkpoint.crc64, checkpoint.trades,
                 checkpoint.active_orders);
    for (std::size_t level = 0; level < checkpoint.bids.size(); ++level) {
      std::fprintf(out, "[%" PRId64 ", %" PRIu64 "]%s", checkpoint.bids[level].first,
                   checkpoint.bids[level].second,
                   level + 1 == checkpoint.bids.size() ? "" : ", ");
    }
    std::fprintf(out, "], \"asks\": [");
    for (std::size_t level = 0; level < checkpoint.asks.size(); ++level) {
      std::fprintf(out, "[%" PRId64 ", %" PRIu64 "]%s", checkpoint.asks[level].first,
                   checkpoint.asks[level].second,
                   level + 1 == checkpoint.asks.size() ? "" : ", ");
    }
    std::fprintf(out, "]}%s\n", i + 1 == result.checkpoints.size() ? "" : ",");
  }
  std::fprintf(out, "  ]\n}\n");
}

int usage() {
  std::printf(
      "UltraLOB CLI\n"
      "  ultralob_cli replay --events N --seed S --workload balanced|cancel_heavy|burst\n"
      "                      [--checkpoint K] [--state-out FILE] [--events-out FILE]\n"
      "  ultralob_cli verify --events N --seed S --workload NAME\n"
      "  ultralob_cli chaos [--json-out FILE]\n");
  return 2;
}

int run_chaos_command(const std::string& json_path) {
  std::vector<ChaosReport> reports(kChaosScenarioCount);
  const std::size_t count = run_all_chaos(reports.data(), reports.size());

  std::FILE* out = stdout;
  if (!json_path.empty()) {
    out = std::fopen(json_path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", json_path.c_str());
      return 1;
    }
  }

  std::fprintf(out, "{\"generated_at\": \"evidence\", \"scenarios\": [\n");
  for (std::size_t i = 0; i < count; ++i) {
    char json[1536];
    chaos_report_to_json(reports[i], json, sizeof(json));
    std::fprintf(out, "  %s%s\n", json, i + 1 == count ? "" : ",");
  }
  std::fprintf(out, "]}\n");
  if (out != stdout) std::fclose(out);

  for (std::size_t i = 0; i < count; ++i) {
    std::printf("[%s] %-20s %s\n", reports[i].passed ? "PASS" : "FAIL",
                chaos_scenario_id(reports[i].scenario), reports[i].detail);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string command = argv[1];

  if (command == "chaos") {
    std::string json_path;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--json-out") == 0 && i + 1 < argc) json_path = argv[++i];
    }
    return run_chaos_command(json_path);
  }

  Options options{};
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (arg == "--events") {
      options.events = std::strtoull(next(), nullptr, 10);
    } else if (arg == "--seed") {
      options.seed = std::strtoull(next(), nullptr, 10);
    } else if (arg == "--workload") {
      options.workload = next();
    } else if (arg == "--checkpoint") {
      options.checkpoint = std::strtoull(next(), nullptr, 10);
    } else if (arg == "--state-out") {
      options.state_out = next();
    } else if (arg == "--events-out") {
      options.events_out = next();
    } else {
      return usage();
    }
  }

  if (command == "verify") {
    const ReplayResult first = run_stream(options, nullptr);
    const ReplayResult second = run_stream(options, nullptr);
    const bool identical = first.final_state.crc64 == second.final_state.crc64 &&
                           first.checkpoints.size() == second.checkpoints.size();
    std::printf("run A crc64=%" PRIu64 " run B crc64=%" PRIu64 " -> %s\n",
                first.final_state.crc64, second.final_state.crc64,
                identical ? "IDENTICAL" : "DIVERGED");
    return identical ? 0 : 1;
  }

  if (command != "replay") return usage();

  std::FILE* events_dump = nullptr;
  if (!options.events_out.empty()) {
    events_dump = std::fopen(options.events_out.c_str(), "w");
    if (events_dump == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", options.events_out.c_str());
      return 1;
    }
  }

  const ReplayResult result = run_stream(options, events_dump);
  if (events_dump != nullptr) std::fclose(events_dump);

  if (!options.state_out.empty()) {
    std::FILE* out = std::fopen(options.state_out.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", options.state_out.c_str());
      return 1;
    }
    write_state_json(result, options, out);
    std::fclose(out);
  } else {
    write_state_json(result, options, stdout);
  }

  std::fprintf(stderr,
               "replay: %" PRIu64 " events, %zu checkpoints, final crc64=%" PRIu64
               ", trades=%" PRIu64 ", rejected=%" PRIu64 "\n",
               options.events, result.checkpoints.size(), result.final_state.crc64,
               result.final_state.trades, result.stats.engine_rejected);
  return 0;
}
