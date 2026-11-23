#include <cstring>

#include "ultralob/matching_engine.hpp"
#include "ultralob/snapshot.hpp"

namespace ultralob {
namespace {

std::size_t image_bytes(std::size_t order_count) {
  if (order_count > (SIZE_MAX - sizeof(BookImageHeader)) / sizeof(BookImageOrder)) {
    return SIZE_MAX;  // saturate rather than wrap into a "fits" buffer size
  }
  return sizeof(BookImageHeader) + order_count * sizeof(BookImageOrder);
}

}  // namespace

std::size_t book_image_size(const MatchingEngine& engine) noexcept {
  return image_bytes(engine.active_orders());
}

bool encode_book_image(const MatchingEngine& engine, Seq seq, void* buffer,
                       std::size_t size, std::size_t* bytes_written) noexcept {
  if (buffer == nullptr) return false;

  // Count the records the walk below will actually write. The header count is
  // derived from the pool, the record count from the FIFO links; if those ever
  // disagree the write would run past the caller's buffer, so refuse instead.
  std::size_t record_count = 0;
  for (const Side side : {Side::Buy, Side::Sell}) {
    for (std::size_t i = 0; i < engine.depth(side); ++i) record_count += engine.level_order_count(side, i);
  }
  const std::size_t needed = image_bytes(record_count);
  if (needed == SIZE_MAX || size < needed) return false;
  if (record_count != engine.active_orders()) return false;

  BookImageHeader header{};
  header.order_count = static_cast<std::uint32_t>(record_count);
  header.seq = seq;
  header.crc64 = engine.crc64();
  header.bids_depth = engine.depth(Side::Buy);
  header.asks_depth = engine.depth(Side::Sell);
  header.trades = engine.trade_count();
  for (const Side side : {Side::Buy, Side::Sell}) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < engine.depth(side); ++i) {
      const PriceLevel* level = engine.level(side, i);
      if (level != nullptr) total += level->total_qty;
    }
    (side == Side::Buy ? header.bid_total : header.ask_total) = total;
  }

  auto* bytes = static_cast<std::uint8_t*>(buffer);
  std::memcpy(bytes, &header, sizeof(header));
  std::size_t offset = sizeof(header);

  // Serialize in the book's own walk order (bids best-to-worst, then asks,
  // FIFO inside each level). Restoring in that order rebuilds identical FIFO
  // priority, so the restored checksum must equal header.crc64.
  const Side sides[2] = {Side::Buy, Side::Sell};
  for (const Side side : sides) {
    for (std::size_t i = 0; i < engine.depth(side); ++i) {
      const std::size_t count = engine.level_order_count(side, i);
      for (std::size_t k = 0; k < count; ++k) {
        const Order* order = engine.level_order_at(side, i, k);
        if (order == nullptr) continue;
        BookImageOrder record{};
        record.id = order->id;
        record.price = order->price;
        record.remaining = order->remaining;
        record.seq = order->seq;
        record.side = static_cast<std::uint8_t>(side);
        std::memcpy(bytes + offset, &record, sizeof(record));
        offset += sizeof(record);
      }
    }
  }

  if (offset != needed) return false;  // never report a partial image as complete
  if (bytes_written != nullptr) *bytes_written = offset;
  return true;
}

bool restore_book_image(MatchingEngine& engine, const void* buffer, std::size_t size,
                        std::uint64_t* out_seq) noexcept {
  if (buffer == nullptr || size < sizeof(BookImageHeader)) return false;

  BookImageHeader header{};
  std::memcpy(&header, buffer, sizeof(header));
  if (header.version != kBookImageVersion) return false;
  if (size < image_bytes(header.order_count)) return false;

  engine.reset();
  const auto* bytes = static_cast<const std::uint8_t*>(buffer);
  std::size_t offset = sizeof(header);
  for (std::uint32_t i = 0; i < header.order_count; ++i) {
    BookImageOrder record{};
    std::memcpy(&record, bytes + offset, sizeof(record));
    offset += sizeof(record);
    if (!engine.restore_resting_order(static_cast<Side>(record.side),
                                      static_cast<Price>(record.price),
                                      static_cast<Qty>(record.remaining),
                                      static_cast<Seq>(record.seq),
                                      static_cast<OrderId>(record.id))) {
      engine.reset();
      return false;
    }
  }

  if (engine.crc64() != header.crc64) {
    engine.reset();
    return false;
  }
  engine.restore_trade_sequence(header.trades);
  if (out_seq != nullptr) *out_seq = header.seq;
  return true;
}

}  // namespace ultralob
