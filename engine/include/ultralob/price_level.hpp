// UltraLOB — price level.
//
// A level owns the aggregate quantity at a price plus an intrusive FIFO of
// order pool indices. Level tables are sorted arrays (see level_table.hpp), so
// the best price is always index 0 and walking the book is a linear scan over
// contiguous memory.
#pragma once

#include "ultralob/types.hpp"

namespace ultralob {

struct PriceLevel {
  Price price = 0;
  Qty total_qty = 0;
  std::uint32_t order_count = 0;
  std::uint32_t head = kNoIndex;  // oldest order (next to match)
  std::uint32_t tail = kNoIndex;  // newest order (last in priority)
};

}  // namespace ultralob
