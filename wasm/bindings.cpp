// UltraLOB — WebAssembly bindings.
//
// This is a thin C ABI over the *real* C++20 engine (engine/src/*.cpp). The
// browser never re-implements matching: it calls these exports from a Web
// Worker and reads results out of two flat static buffers, so no per-call
// allocation, no JSON on the hot path and no embind overhead.
//
// Out-buffer layout (documented once here, mirrored in web/src/wasm/engine.ts):
//   book levels   u64[2*i + 0] = quantity, u64[2*i + 1] = order count,
//                 f64[i]       = price in ticks
//   trades        u64[6*i + 0..5] = trade id, maker id, taker id, quantity,
//                 sequence, taker side (0 buy / 1 sell); f64[i] = price ticks
//   benchmark     u64[0..]: events, trades, rejected, active orders, crc64,
//                 elapsed ns, p50, p99, p99.9, p99.99, max, mean
//                 f64[0] = end-to-end events/second, f64[1] = match ns/second
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "ultralob/chaos.hpp"
#include "ultralob/config.hpp"
#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/latency_histogram.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/replay.hpp"
#include "ultralob/snapshot.hpp"

namespace {

using namespace ultralob;

constexpr int kOutSlots = 8192;

std::unique_ptr<MatchingEngine> g_engine;
std::unique_ptr<EventProcessor> g_processor;

std::unique_ptr<MatchingEngine> g_replay_engine;
std::unique_ptr<ReplaySession> g_replay;

// The exported accessor is declared `unsigned long long*` — the JS layer reads
// 64-bit words through it. On LP64 Linux `uint64_t` is `unsigned long` (a
// different type), so the buffer must use the exact exported type or the native
// build fails to compile even though the Emscripten build happens to agree.
unsigned long long g_out_u64[kOutSlots];
double g_out_f64[kOutSlots];
char g_text[8192];

Seq g_seq = 0;
std::uint64_t g_event_id = 0;
OrderId g_next_order_id = 1;
OrderId g_last_order_id = 0;
int g_last_status = 0;
int g_last_reject = 0;

void ensure_engine() {
  if (!g_engine) {
    g_engine = std::make_unique<MatchingEngine>();
    g_processor = std::make_unique<EventProcessor>(*g_engine);
  }
}

EventProcessor::Config processor_config(GapPolicy gap_policy) {
  EventProcessor::Config config{};
  config.gap_policy = gap_policy;
  config.duplicate_policy = DuplicatePolicy::Reject;
  config.start_seq = 1;
  return config;
}

ProcessOutcome submit(Event event) {
  ensure_engine();
  event.seq = ++g_seq;
  event.id = ++g_event_id;
  event.ts = event.seq * 1000ull;
  const ProcessOutcome outcome = g_processor->process(event);
  g_last_status = static_cast<int>(outcome.status);
  g_last_reject = static_cast<int>(outcome.reason);
  return outcome;
}

int depth_levels(const MatchingEngine* engine, int side, int max_levels) {
  if (engine == nullptr || max_levels <= 0) return 0;
  const Side book_side = side == 0 ? Side::Buy : Side::Sell;
  int count = 0;
  const auto depth = static_cast<int>(engine->depth(book_side));
  const int limit = depth < max_levels ? depth : max_levels;
  for (int i = 0; i < limit; ++i) {
    const PriceLevel* level = engine->level(book_side, static_cast<std::size_t>(i));
    if (level == nullptr) break;
    g_out_f64[count] = static_cast<double>(level->price);
    g_out_u64[count * 2] = level->total_qty;
    g_out_u64[count * 2 + 1] = level->order_count;
    ++count;
  }
  return count;
}

}  // namespace

extern "C" {

// --- lifecycle -------------------------------------------------------------
void ul_init(unsigned seed) {
  (void)seed;
  ensure_engine();
  g_engine->reset();
  g_processor->reset(processor_config(GapPolicy::Buffer));
  g_seq = 0;
  g_event_id = 0;
  g_next_order_id = 1;
  g_last_order_id = 0;
  g_last_status = 0;
  g_last_reject = 0;
}

unsigned long long* ul_out_u64() { return g_out_u64; }
double* ul_out_f64() { return g_out_f64; }

// --- interactive order entry (routed through the event processor) ----------
int ul_submit_limit(int side, int price_ticks, int qty) {
  ensure_engine();
  const OrderId id = ++g_next_order_id;
  Event event{};
  event.type = EventType::NewOrder;
  event.side = side == 0 ? Side::Buy : Side::Sell;
  event.order_type = OrderType::Limit;
  event.price = price_ticks;
  event.qty = qty < 0 ? 0 : static_cast<Qty>(qty);
  event.order_id = id;
  const ProcessOutcome outcome = submit(event);
  if (outcome.status != ProcessStatus::Ok) return 0;
  g_last_order_id = id;
  return static_cast<int>(id);
}

int ul_submit_market(int side, int qty) {
  ensure_engine();
  const OrderId id = ++g_next_order_id;
  Event event{};
  event.type = EventType::NewOrder;
  event.side = side == 0 ? Side::Buy : Side::Sell;
  event.order_type = OrderType::Market;
  event.qty = qty < 0 ? 0 : static_cast<Qty>(qty);
  event.order_id = id;
  const ProcessOutcome outcome = submit(event);
  if (outcome.status != ProcessStatus::Ok) return 0;
  g_last_order_id = id;
  return static_cast<int>(id);
}

int ul_cancel(unsigned order_id) {
  Event event{};
  event.type = EventType::CancelOrder;
  event.order_id = order_id;
  return submit(event).status == ProcessStatus::Ok ? 1 : 0;
}

int ul_modify(unsigned order_id, int price_ticks, int qty) {
  Event event{};
  event.type = EventType::ModifyOrder;
  event.order_id = order_id;
  event.price = price_ticks;
  event.qty = qty < 0 ? 0 : static_cast<Qty>(qty);
  return submit(event).status == ProcessStatus::Ok ? 1 : 0;
}

int ul_last_status() { return g_last_status; }
int ul_last_reject() { return g_last_reject; }
unsigned long long ul_last_order_id() {
  return static_cast<unsigned long long>(g_last_order_id);
}

// --- market data -----------------------------------------------------------
int ul_book_side(int side, int max_levels) {
  ensure_engine();
  return depth_levels(g_engine.get(), side, max_levels);
}

int ul_trades(int max) {
  ensure_engine();
  if (max <= 0) return 0;
  // The out buffer holds kOutSlots / 6 trades (6 u64 words each). The scratch
  // window must be at least that large: a fixed 600-entry stack array was a
  // stack overflow as soon as a caller asked for more than 600 trades.
  constexpr int kMaxTradesOut = kOutSlots / 6;
  const int limit = max > kMaxTradesOut ? kMaxTradesOut : max;
  static Trade window[kMaxTradesOut];
  const int count = static_cast<int>(g_engine->recent_trades(window, static_cast<std::size_t>(limit)));
  for (int i = 0; i < count; ++i) {
    g_out_f64[i] = static_cast<double>(window[i].price);
    g_out_u64[i * 6 + 0] = window[i].id;
    g_out_u64[i * 6 + 1] = window[i].maker;
    g_out_u64[i * 6 + 2] = window[i].taker;
    g_out_u64[i * 6 + 3] = window[i].qty;
    g_out_u64[i * 6 + 4] = window[i].seq;
    g_out_u64[i * 6 + 5] = window[i].taker_side == Side::Buy ? 0 : 1;
  }
  return count;
}

double ul_best_bid() {
  ensure_engine();
  return g_engine->has_bid() ? static_cast<double>(g_engine->best_bid()) : 0.0;
}

double ul_best_ask() {
  ensure_engine();
  return g_engine->has_ask() ? static_cast<double>(g_engine->best_ask()) : 0.0;
}

double ul_spread() {
  ensure_engine();
  const Price spread = g_engine->spread();
  return spread < 0 ? -1.0 : static_cast<double>(spread);
}

int ul_has_order(unsigned order_id) {
  ensure_engine();
  return g_engine->find(order_id) != nullptr ? 1 : 0;
}

double ul_order_remaining(unsigned order_id) {
  ensure_engine();
  const Order* order = g_engine->find(order_id);
  return order == nullptr ? 0.0 : static_cast<double>(order->remaining);
}

int ul_side_depth(int side) {
  ensure_engine();
  return static_cast<int>(g_engine->depth(side == 0 ? Side::Buy : Side::Sell));
}

unsigned long long ul_seq() { return static_cast<unsigned long long>(g_seq); }
unsigned long long ul_crc64() {
  ensure_engine();
  return static_cast<unsigned long long>(g_engine->crc64());
}
unsigned long long ul_active_orders() {
  ensure_engine();
  return static_cast<unsigned long long>(g_engine->active_orders());
}
unsigned long long ul_trade_count() {
  ensure_engine();
  return static_cast<unsigned long long>(g_engine->trade_count());
}
unsigned long long ul_processed() {
  ensure_engine();
  return static_cast<unsigned long long>(g_processor->stats().applied);
}
unsigned long long ul_rejected() {
  ensure_engine();
  return static_cast<unsigned long long>(g_processor->stats().engine_rejected);
}
unsigned long long ul_duplicates() {
  ensure_engine();
  return static_cast<unsigned long long>(g_processor->stats().duplicates);
}
unsigned long long ul_gaps() {
  ensure_engine();
  return static_cast<unsigned long long>(g_processor->stats().gaps);
}

// --- benchmark (same measurement approach as the native harness) -----------
int ul_benchmark(unsigned events, unsigned seed, int workload_id) {
  const Workload workload = static_cast<Workload>(
      workload_id < 0 ? 0 : (workload_id > 2 ? 2 : workload_id));
  const FeedConfig feed = workload_config(workload, seed);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, processor_config(GapPolicy::Buffer));
  FeedGenerator generator(feed);

  LatencyHistogram histogram;
  const auto started_all = std::chrono::steady_clock::now();
  std::uint64_t matching_ns = 0;
  std::uint64_t rejected = 0;
  for (unsigned i = 0; i < events; ++i) {
    const Event event = generator.next();
    const auto started = std::chrono::steady_clock::now();
    const ProcessOutcome outcome = processor.process(event);
    const auto finished = std::chrono::steady_clock::now();
    const std::uint64_t ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    histogram.record(ns);
    matching_ns += ns;
    if (outcome.status == ProcessStatus::Rejected) ++rejected;
  }
  const auto finished_all = std::chrono::steady_clock::now();
  const std::uint64_t wall_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished_all - started_all)
          .count());

  std::memset(g_out_u64, 0, sizeof(g_out_u64));
  std::memset(g_out_f64, 0, sizeof(g_out_f64));
  g_out_u64[0] = events;
  g_out_u64[1] = engine->trade_count();
  g_out_u64[2] = rejected;
  g_out_u64[3] = engine->active_orders();
  g_out_u64[4] = engine->crc64();
  g_out_u64[5] = wall_ns;
  g_out_u64[6] = histogram.percentile(50.0);
  g_out_u64[7] = histogram.percentile(99.0);
  g_out_u64[8] = histogram.percentile(99.9);
  g_out_u64[9] = histogram.percentile(99.99);
  g_out_u64[10] = histogram.max();
  g_out_u64[11] = histogram.mean();
  g_out_f64[0] = wall_ns == 0 ? 0.0 : static_cast<double>(events) * 1e9 / static_cast<double>(wall_ns);
  g_out_f64[1] = matching_ns == 0
                     ? 0.0
                     : static_cast<double>(events) * 1e9 / static_cast<double>(matching_ns);
  return 0;
}

// --- deterministic replay lab ---------------------------------------------
void ul_replay_begin(unsigned events, unsigned seed, int workload_id, int gap_policy) {
  const Workload workload = static_cast<Workload>(
      workload_id < 0 ? 0 : (workload_id > 2 ? 2 : workload_id));
  ReplayConfig config{};
  config.feed = workload_config(workload, seed);
  config.processor = processor_config(
      gap_policy == 0 ? GapPolicy::RejectAndHalt
                      : (gap_policy == 1 ? GapPolicy::RejectAndContinue : GapPolicy::Buffer));
  config.processor.start_seq = 1;

  g_replay_engine = std::make_unique<MatchingEngine>();
  g_replay = std::make_unique<ReplaySession>(*g_replay_engine, config);
  (void)events;
}

int ul_replay_step(unsigned steps) {
  if (!g_replay) return -1;
  int last = 0;
  for (unsigned i = 0; i < steps; ++i) {
    const ProcessOutcome outcome = g_replay->step();
    last = static_cast<int>(outcome.status);
    if (outcome.status == ProcessStatus::Halted) break;
  }
  return last;
}

unsigned long long ul_replay_generated() {
  return g_replay ? static_cast<unsigned long long>(g_replay->generated()) : 0ull;
}

unsigned long long ul_replay_crc64() {
  return g_replay_engine ? static_cast<unsigned long long>(g_replay_engine->crc64()) : 0ull;
}

unsigned long long ul_replay_trades() {
  return g_replay_engine ? static_cast<unsigned long long>(g_replay_engine->trade_count())
                         : 0ull;
}

int ul_replay_gaps() {
  return g_replay ? static_cast<int>(g_replay->processor().stats().gaps) : 0;
}

int ul_replay_book_side(int side, int max_levels) {
  return depth_levels(g_replay_engine.get(), side, max_levels);
}

// Peek at the next generated event without consuming it.
int ul_replay_next_event() {
  if (!g_replay) return 0;
  const Event event = g_replay->peek_next();
  g_out_u64[0] = static_cast<std::uint64_t>(event.type);
  g_out_u64[1] = static_cast<std::uint64_t>(event.side);
  g_out_u64[2] = static_cast<std::uint64_t>(event.order_type);
  g_out_u64[3] = event.order_id;
  g_out_u64[4] = event.seq;
  g_out_u64[5] = event.qty;
  g_out_f64[0] = static_cast<double>(event.price);
  return 1;
}

// Independent second execution of the same stream: proves the lab's checksum
// comparison is a real cross-run comparison, not an echo of the live value.
int ul_replay_verify() {
  if (!g_replay) return 0;
  const ReplayConfig config = g_replay->config();
  const Seq target = g_replay->generated();

  auto reference_engine = std::make_unique<MatchingEngine>();
  ReplaySession reference(*reference_engine, config);
  reference.run(target);

  const std::uint64_t live_crc = g_replay_engine->crc64();
  const std::uint64_t reference_crc = reference_engine->crc64();

  g_out_u64[0] = live_crc;
  g_out_u64[1] = reference_crc;
  g_out_u64[2] = static_cast<std::uint64_t>(target);
  return live_crc == reference_crc ? 1 : 0;
}

// --- reliability lab -------------------------------------------------------
const char* ul_chaos_json(int scenario) {
  std::size_t index = 0;
  if (scenario >= 0) index = static_cast<std::size_t>(scenario);
  if (index >= kChaosScenarioCount) index = 0;
  const ChaosReport report = run_chaos_scenario(static_cast<ChaosScenario>(index));
  chaos_report_to_json(report, g_text, sizeof(g_text));
  return g_text;
}

// Builds the array without trusting printf's return value. The previous version
// advanced a size_t offset by snprintf()'s result; a negative return wrapped the
// offset to SIZE_MAX and every subsequent write landed outside the buffer, so
// the browser received an empty string and JSON.parse threw. Writes here are
// bounds-checked and always leave a terminated, syntactically closed array.
const char* ul_chaos_all_json() {
  constexpr std::size_t kSize = sizeof(g_text);
  std::size_t written = 0;
  g_text[0] = '\0';

  auto append = [&](const char* text) -> bool {
    const std::size_t len = std::strlen(text);
    // Keep room for the closing ']' and the terminator.
    if (written + len + 2 > kSize) return false;
    std::memcpy(g_text + written, text, len);
    written += len;
    g_text[written] = '\0';
    return true;
  };

  if (!append("[")) return g_text;

  bool any = false;
  for (std::size_t i = 0; i < kChaosScenarioCount; ++i) {
    char report[2048];
    chaos_report_to_json(run_chaos_scenario(static_cast<ChaosScenario>(i)), report,
                         sizeof(report));
    if (any && !append(",")) break;
    if (!append(report)) {
      if (any) {
        written -= 1;  // drop the separator so the array stays valid
        g_text[written] = '\0';
      }
      break;
    }
    any = true;
  }

  append("]");
  return g_text;
}

}  // extern "C"
