#include "ultralob/snapshot.hpp"

#include <cstdio>
#include <cstring>

namespace ultralob {

bool Snapshot::equals(const Snapshot& other) const noexcept {
  if (seq != other.seq || crc64 != other.crc64 || applied_events != other.applied_events ||
      trades != other.trades || bid_levels != other.bid_levels ||
      ask_levels != other.ask_levels || bid_total != other.bid_total ||
      ask_total != other.ask_total) {
    return false;
  }
  for (std::size_t i = 0; i < Limits::kSnapshotLevels; ++i) {
    if (bids[i].price != other.bids[i].price || bids[i].qty != other.bids[i].qty ||
        bids[i].orders != other.bids[i].orders) {
      return false;
    }
    if (asks[i].price != other.asks[i].price || asks[i].qty != other.asks[i].qty ||
        asks[i].orders != other.asks[i].orders) {
      return false;
    }
  }
  return true;
}

void Snapshot::diff(const Snapshot& expected, char* out, std::size_t out_size) const noexcept {
  if (out == nullptr || out_size == 0) return;
  out[0] = '\0';
  if (equals(expected)) return;

  std::size_t written = 0;
  auto append = [&](const char* fmt, auto... args) {
    if (written >= out_size) return;
    const int n = std::snprintf(out + written, out_size - written, fmt, args...);
    if (n > 0) written += static_cast<std::size_t>(n);
    if (written > out_size) written = out_size;
  };

  if (crc64 != expected.crc64) append("crc64 actual=%llu expected=%llu; ",
                                      static_cast<unsigned long long>(crc64),
                                      static_cast<unsigned long long>(expected.crc64));
  if (seq != expected.seq) append("seq actual=%llu expected=%llu; ",
                                  static_cast<unsigned long long>(seq),
                                  static_cast<unsigned long long>(expected.seq));
  if (trades != expected.trades) append("trades actual=%llu expected=%llu; ",
                                        static_cast<unsigned long long>(trades),
                                        static_cast<unsigned long long>(expected.trades));
  if (bid_total != expected.bid_total) append("bid_total actual=%llu expected=%llu; ",
                                              static_cast<unsigned long long>(bid_total),
                                              static_cast<unsigned long long>(expected.bid_total));
  if (ask_total != expected.ask_total) append("ask_total actual=%llu expected=%llu; ",
                                              static_cast<unsigned long long>(ask_total),
                                              static_cast<unsigned long long>(expected.ask_total));
  for (std::size_t i = 0; i < Limits::kSnapshotLevels; ++i) {
    if (bids[i].price != expected.bids[i].price || bids[i].qty != expected.bids[i].qty) {
      append("bid_level[%zu] actual=%lld/%llu expected=%lld/%llu; ", i,
             static_cast<long long>(bids[i].price),
             static_cast<unsigned long long>(bids[i].qty),
             static_cast<long long>(expected.bids[i].price),
             static_cast<unsigned long long>(expected.bids[i].qty));
    }
    if (asks[i].price != expected.asks[i].price || asks[i].qty != expected.asks[i].qty) {
      append("ask_level[%zu] actual=%lld/%llu expected=%lld/%llu; ", i,
             static_cast<long long>(asks[i].price),
             static_cast<unsigned long long>(asks[i].qty),
             static_cast<long long>(expected.asks[i].price),
             static_cast<unsigned long long>(expected.asks[i].qty));
    }
  }
}

Snapshot take_snapshot(const MatchingEngine& engine, std::uint64_t applied_events,
                       Seq seq) noexcept {
  Snapshot snapshot{};
  snapshot.seq = seq;
  snapshot.crc64 = engine.crc64();
  snapshot.applied_events = applied_events;
  snapshot.trades = engine.trade_count();

  const Side sides[2] = {Side::Buy, Side::Sell};
  for (const Side side : sides) {
    SnapshotLevel* out = side == Side::Buy ? snapshot.bids : snapshot.asks;
    std::size_t& count = side == Side::Buy ? snapshot.bid_levels : snapshot.ask_levels;
    Qty& total = side == Side::Buy ? snapshot.bid_total : snapshot.ask_total;
    const std::size_t depth = engine.depth(side);
    count = depth;
    for (std::size_t i = 0; i < depth; ++i) {
      const PriceLevel* level = engine.level(side, i);
      if (level == nullptr) break;
      total += level->total_qty;
      if (i < Limits::kSnapshotLevels) {
        out[i].price = level->price;
        out[i].qty = level->total_qty;
        out[i].orders = level->order_count;
      }
    }
  }
  return snapshot;
}

}  // namespace ultralob
