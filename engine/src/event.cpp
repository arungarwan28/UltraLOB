#include "ultralob/event.hpp"

namespace ultralob {

const char* event_type_name(EventType type) noexcept {
  switch (type) {
    case EventType::NewOrder: return "NEW_ORDER";
    case EventType::CancelOrder: return "CANCEL_ORDER";
    case EventType::ModifyOrder: return "MODIFY_ORDER";
    case EventType::TradeExecuted: return "TRADE_EXECUTED";
    case EventType::SnapshotCreated: return "SNAPSHOT_CREATED";
    case EventType::GapDetected: return "GAP_DETECTED";
    case EventType::RecoveryCompleted: return "RECOVERY_COMPLETED";
  }
  return "UNKNOWN";
}

bool is_mutating(EventType type) noexcept {
  return type == EventType::NewOrder || type == EventType::CancelOrder ||
         type == EventType::ModifyOrder;
}

}  // namespace ultralob
