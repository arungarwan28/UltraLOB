// UltraLOB — canonical book snapshot.
//
// A snapshot is the unit of comparison for determinism: sequence, checksum,
// top-of-book levels and aggregate depth. Two replays are "the same" only if
// this structure compares equal.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ultralob/config.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

struct SnapshotLevel {
  Price price = 0;
  Qty qty = 0;
  std::uint32_t orders = 0;
};

struct Snapshot {
  Seq seq = 0;                  // last applied event sequence
  std::uint64_t crc64 = 0;
  std::uint64_t applied_events = 0;
  std::uint64_t trades = 0;
  std::size_t bid_levels = 0;
  std::size_t ask_levels = 0;
  SnapshotLevel bids[Limits::kSnapshotLevels]{};
  SnapshotLevel asks[Limits::kSnapshotLevels]{};
  Qty bid_total = 0;
  Qty ask_total = 0;

  bool equals(const Snapshot& other) const noexcept;
  // Human/machine readable difference report ("" when equal).
  void diff(const Snapshot& expected, char* out, std::size_t out_size) const noexcept;
};

// Captures the top `Limits::kSnapshotLevels` levels plus aggregate depth.
Snapshot take_snapshot(const MatchingEngine& engine, std::uint64_t applied_events,
                       Seq seq) noexcept;

// ---------------------------------------------------------------------------
// Book image: a serializable, restorable snapshot of every resting order.
//
// Layout (little endian, packed):
//   BookImageHeader { u32 version, u32 order_count, u64 seq, u64 crc64,
//                     u64 bids_depth, u64 asks_depth, u64 bid_total,
//                     u64 ask_total, u64 trades, u64 pad }
//   then order_count records, ordered exactly as the book walks itself
//   (bids best-to-worst then asks, FIFO within each level):
//   BookImageOrder  { u64 id, i64 price, u64 remaining, u64 seq, u8 side,
//                     u8 reserved[7] }
// A restored image reproduces the same checksum, which is what makes snaphot
// recovery verifiable rather than decorative.
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kBookImageVersion = 1;

struct BookImageHeader {
  std::uint32_t version = kBookImageVersion;
  std::uint32_t order_count = 0;
  std::uint64_t seq = 0;
  std::uint64_t crc64 = 0;
  std::uint64_t bids_depth = 0;
  std::uint64_t asks_depth = 0;
  std::uint64_t bid_total = 0;
  std::uint64_t ask_total = 0;
  std::uint64_t trades = 0;  // trade counter at checkpoint (tape not serialized)
  std::uint64_t reserved = 0;
};

struct BookImageOrder {
  std::uint64_t id = 0;
  std::int64_t price = 0;
  std::uint64_t remaining = 0;
  std::uint64_t seq = 0;
  std::uint8_t side = 0;
  std::uint8_t reserved[7]{};
};

// Bytes needed to encode the current book (header + records).
std::size_t book_image_size(const MatchingEngine& engine) noexcept;

// Encodes the book. Returns false when the buffer is too small.
bool encode_book_image(const MatchingEngine& engine, Seq seq, void* buffer,
                       std::size_t size, std::size_t* bytes_written) noexcept;

// Restores a book into `engine` (which is reset first). Returns false on a
// version, size or checksum mismatch. On success the engine's crc64() equals
// the crc64 stored in the image header.
bool restore_book_image(MatchingEngine& engine, const void* buffer, std::size_t size,
                        std::uint64_t* out_seq) noexcept;

}  // namespace ultralob
