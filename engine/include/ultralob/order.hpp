// UltraLOB — order record.
//
// Orders live in a fixed-capacity pool and are linked into their price level's
// FIFO queue by *index*, not pointer. Index links keep the layout stable when
// the pool grows or is copied, make state serialization (and therefore the
// checksum) trivially canonical, and avoid allocator involvement entirely.
#pragma once

#include "ultralob/types.hpp"

namespace ultralob {

struct Order {
  OrderId id = kInvalidId;
  Price price = 0;
  Qty qty = 0;        // original submitted quantity
  Qty remaining = 0;  // unfilled quantity (leaves the book at zero)
  Seq seq = 0;        // arrival sequence, retained for audit/priority reasoning
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  OrderStatus status = OrderStatus::New;
  std::uint32_t prev = kNoIndex;   // FIFO predecessor (towards the head)
  std::uint32_t next = kNoIndex;   // FIFO successor (towards the tail)
  std::uint32_t slot = kNoIndex;   // own pool slot
};

inline constexpr std::size_t kOrderRecordBytes = sizeof(Order);

}  // namespace ultralob
