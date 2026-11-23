// UltraLOB — event model.
#pragma once

#include <cstdint>

#include "ultralob/types.hpp"

namespace ultralob {

enum class EventType : std::uint8_t {
  NewOrder = 1,
  CancelOrder = 2,
  ModifyOrder = 3,
  TradeExecuted = 4,
  SnapshotCreated = 5,
  GapDetected = 6,
  RecoveryCompleted = 7,
};

const char* event_type_name(EventType type) noexcept;
bool is_mutating(EventType type) noexcept;

struct Event {
  Seq seq = 0;              // expected position in the stream (1-based)
  std::uint64_t id = 0;     // event id used for duplicate detection (0 = none)
  Timestamp ts = 0;         // simulated nanosecond timestamp
  EventType type = EventType::NewOrder;
  Side side = Side::Buy;
  OrderType order_type = OrderType::Limit;
  Price price = 0;
  Qty qty = 0;
  OrderId order_id = kInvalidId;
};

}  // namespace ultralob
