#!/usr/bin/env python3
"""UltraLOB — Python/C++ parity test.

The C++ engine replays a deterministic generated stream and emits, every
`--checkpoint` events, the top five bid and ask levels plus totals. This script
replays the *same* event stream (dumped by the C++ CLI) through an independent
Python reference book and compares the two views.

The reference implementation is intentionally naive (dict of price -> deque) so
that it shares no code and no data structures with the engine. Matching rules
mirrored on purpose:

  * price then time priority, integer tick prices
  * limit orders match while they cross, remainder rests
  * market orders never rest (unfilled remainder cancelled, IOC)
  * modify keeps queue priority only when price is unchanged and quantity does
    not increase; anything else re-enters at the tail and may trade
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from collections import deque

CHECKPOINT_LEVELS = 5


class ReferenceBook:
    def __init__(self) -> None:
        self.bids: dict[int, deque[tuple[int, int]]] = {}
        self.asks: dict[int, deque[tuple[int, int]]] = {}
        self.remaining: dict[int, int] = {}
        self.side_of: dict[int, int] = {}
        self.price_of: dict[int, int] = {}
        self.trades = 0

    # -- helpers ---------------------------------------------------------
    def _levels(self, side: int) -> dict[int, deque[tuple[int, int]]]:
        return self.bids if side == 0 else self.asks

    def _best_price(self, side: int) -> int | None:
        levels = self._levels(side)
        if not levels:
            return None
        return max(levels) if side == 0 else min(levels)

    def _cancels(self, taker_side: int, price: int, is_market: bool) -> bool:
        if is_market:
            return True
        return price <= self._best_price(taker_side) if taker_side == 0 else (
            price >= self._best_price(taker_side)
        )

    def _match(self, taker_side: int, limit_price: int, qty: int, is_market: bool) -> int:
        opposite = 1 - taker_side
        while qty > 0:
            best = self._best_price(opposite)
            if best is None:
                break
            if not is_market:
                if taker_side == 0 and best > limit_price:
                    break
                if taker_side == 1 and best < limit_price:
                    break
            level = self._levels(opposite)[best]
            while qty > 0 and level:
                order_id, resting = level[0]
                fill = min(qty, resting)
                qty -= fill
                resting -= fill
                self.trades += 1
                if resting == 0:
                    level.popleft()
                    del self.remaining[order_id]
                    del self.side_of[order_id]
                    del self.price_of[order_id]
                else:
                    level[0] = (order_id, resting)
                    self.remaining[order_id] = resting
            if not level:
                del self._levels(opposite)[best]
        return qty

    # -- operations ------------------------------------------------------
    def submit_limit(self, order_id: int, side: int, price: int, qty: int) -> bool:
        if qty <= 0 or order_id in self.remaining:
            return False
        leftover = self._match(side, price, qty, False)
        if leftover > 0:
            self._levels(side).setdefault(price, deque()).append((order_id, leftover))
            self.remaining[order_id] = leftover
            self.side_of[order_id] = side
            self.price_of[order_id] = price
        return True

    def submit_market(self, order_id: int, side: int, qty: int) -> bool:
        if qty <= 0:
            return False
        self._match(side, 0, qty, True)  # remainder is cancelled (IOC)
        return True

    def cancel(self, order_id: int) -> bool:
        if order_id not in self.remaining:
            return False
        side = self.side_of[order_id]
        price = self.price_of[order_id]
        level = self._levels(side)[price]
        survivor = deque(entry for entry in level if entry[0] != order_id)
        if survivor:
            self._levels(side)[price] = survivor
        else:
            del self._levels(side)[price]
        del self.remaining[order_id]
        del self.side_of[order_id]
        del self.price_of[order_id]
        return True

    def modify(self, order_id: int, price: int, qty: int) -> bool:
        if order_id not in self.remaining or qty <= 0:
            return False
        side = self.side_of[order_id]
        old_price = self.price_of[order_id]
        if price == old_price and qty <= self.remaining[order_id]:
            level = self._levels(side)[old_price]
            for index, (resting_id, _) in enumerate(level):
                if resting_id == order_id:
                    level[index] = (order_id, qty)
                    break
            self.remaining[order_id] = qty
            return True
        self.cancel(order_id)
        return self.submit_limit(order_id, side, price, qty)

    # -- views -----------------------------------------------------------
    def top(self, side: int) -> list[list[int]]:
        levels = self._levels(side)
        prices = sorted(levels, reverse=(side == 0))[:CHECKPOINT_LEVELS]
        return [[price, sum(resting for _, resting in levels[price])] for price in prices]

    def active_orders(self) -> int:
        return len(self.remaining)


def replay(events_path: str) -> tuple[ReferenceBook, dict[int, dict]]:
    book = ReferenceBook()
    snapshots: dict[int, dict] = {}
    checkpoint_interval = 0

    with open(events_path, "r", encoding="utf-8") as handle:
        applied = 0
        for line in handle:
            event = json.loads(line)
            applied += 1
            kind = event["type"]
            if kind == "NEW_ORDER":
                if event["order_type"] == "market":
                    book.submit_market(event["order_id"], 0 if event["side"] == "bid" else 1,
                                       event["qty"])
                else:
                    book.submit_limit(event["order_id"], 0 if event["side"] == "bid" else 1,
                                      event["price"], event["qty"])
            elif kind == "CANCEL_ORDER":
                book.cancel(event["order_id"])
            elif kind == "MODIFY_ORDER":
                book.modify(event["order_id"], event["price"], event["qty"])
            snapshots[event["seq"]] = {
                "trades": book.trades,
                "active_orders": book.active_orders(),
                "bids": book.top(0),
                "asks": book.top(1),
            }
            checkpoint_interval = applied
    return book, snapshots


def main() -> int:
    parser = argparse.ArgumentParser(description="UltraLOB C++/Python parity check")
    parser.add_argument("--bin", default=os.path.join("build", "ultralob_cli"))
    parser.add_argument("--events", type=int, default=50000)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--workload", default="balanced")
    parser.add_argument("--checkpoint", type=int, default=10000)
    parser.add_argument("--report", default=os.path.join("reports", "parity_results.json"))
    args = parser.parse_args()

    if not os.path.exists(args.bin):
        print(f"CLI not found: {args.bin} (run `make cli` first)", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        state_path = os.path.join(tmp, "state.json")
        events_path = os.path.join(tmp, "events.jsonl")
        subprocess.run(
            [
                args.bin, "replay",
                "--events", str(args.events),
                "--seed", str(args.seed),
                "--workload", args.workload,
                "--checkpoint", str(args.checkpoint),
                "--state-out", state_path,
                "--events-out", events_path,
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        with open(state_path, "r", encoding="utf-8") as handle:
            cpp_state = json.load(handle)

        book, reference = replay(events_path)

    mismatches: list[str] = []
    compared = 0
    for checkpoint in cpp_state["checkpoints"]:
        seq = checkpoint["seq"]
        observed = reference.get(seq)
        if observed is None:
            mismatches.append(f"seq {seq}: reference produced no state")
            continue
        compared += 1
        if checkpoint["bids"] != observed["bids"]:
            mismatches.append(
                f"seq {seq}: bids cpp={checkpoint['bids']} py={observed['bids']}"
            )
        if checkpoint["asks"] != observed["asks"]:
            mismatches.append(
                f"seq {seq}: asks cpp={checkpoint['asks']} py={observed['asks']}"
            )
        if checkpoint["trades"] != observed["trades"]:
            mismatches.append(
                f"seq {seq}: trades cpp={checkpoint['trades']} py={observed['trades']}"
            )
        if checkpoint["active_orders"] != observed["active_orders"]:
            mismatches.append(
                f"seq {seq}: active cpp={checkpoint['active_orders']} "
                f"py={observed['active_orders']}"
            )

    summary = {
        "test": "cpp_python_parity",
        "events": args.events,
        "seed": args.seed,
        "workload": args.workload,
        "checkpoints_compared": compared,
        "levels_per_side": CHECKPOINT_LEVELS,
        "trades_cpp": cpp_state["final"]["trades"],
        "trades_python": book.trades,
        "active_orders_cpp": cpp_state["final"]["bid_levels"] + cpp_state["final"]["ask_levels"],
        "mismatch_count": len(mismatches),
        "passed": not mismatches and compared > 0,
        "mismatches": mismatches[:10],
    }

    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)
        handle.write("\n")

    if mismatches:
        for mismatch in mismatches[:10]:
            print(f"[MISMATCH] {mismatch}")
        print(f"parity: FAILED ({len(mismatches)} mismatches over {compared} checkpoints)")
        return 1

    print(
        f"parity: OK — {compared} checkpoints, {CHECKPOINT_LEVELS} levels/side, "
        f"{book.trades} trades agree, devices: C++ engine vs Python reference"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
