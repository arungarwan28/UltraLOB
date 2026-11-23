// Typed facade over the UltraLOB WebAssembly module.
//
// Layout contract with wasm/bindings.cpp:
//   u64[2*i]     = level quantity        f64[i]       = level price (ticks)
//   u64[2*i + 1] = level order count
//   u64[6*i + k] = trade id / maker / taker / qty / seq / taker side
//   f64[i]       = trade price (ticks)
//   benchmark:   u64[0..11] = events, trades, rejected, active, crc, elapsed ns,
//                p50, p99, p99.9, p99.99, max, mean
//                f64[0] = events/s end-to-end, f64[1] = matching-only events/s
import type {
  BenchResult,
  ChaosReport,
  EngineState,
  GapPolicy,
  Level,
  ReplayState,
  Side,
  TradeRow,
  VerifyResult,
  Workload,
} from '../lib/types';

export interface UltralobWasm {
  _ul_init(seed: number): void;
  _ul_out_u64(): number;
  _ul_out_f64(): number;
  _ul_submit_limit(side: number, priceTicks: number, qty: number): number;
  _ul_submit_market(side: number, qty: number): number;
  _ul_cancel(orderId: number): number;
  _ul_modify(orderId: number, priceTicks: number, qty: number): number;
  _ul_has_order(orderId: number): number;
  _ul_order_remaining(orderId: number): number;
  _ul_last_status(): number;
  _ul_last_reject(): number;
  _ul_last_order_id(): bigint;
  _ul_book_side(side: number, maxLevels: number): number;
  _ul_trades(max: number): number;
  _ul_best_bid(): number;
  _ul_best_ask(): number;
  _ul_spread(): number;
  _ul_side_depth(side: number): number;
  _ul_seq(): bigint;
  _ul_crc64(): bigint;
  _ul_active_orders(): bigint;
  _ul_trade_count(): bigint;
  _ul_processed(): bigint;
  _ul_rejected(): bigint;
  _ul_duplicates(): bigint;
  _ul_gaps(): bigint;
  _ul_benchmark(events: number, seed: number, workload: number): number;
  _ul_replay_begin(events: number, seed: number, workload: number, gapPolicy: number): void;
  _ul_replay_step(steps: number): number;
  _ul_replay_generated(): bigint;
  _ul_replay_crc64(): bigint;
  _ul_replay_trades(): bigint;
  _ul_replay_gaps(): number;
  _ul_replay_book_side(side: number, maxLevels: number): number;
  _ul_replay_next_event(): number;
  _ul_replay_verify(): number;
  _ul_chaos_json(scenario: number): number;
  _ul_chaos_all_json(): number;
  HEAPU8: Uint8Array;
  HEAPU64: BigUint64Array;
  HEAPF64: Float64Array;
  UTF8ToString(pointer: number): string;
}

const LEVEL_LIMIT = 18;
const TAPE_LIMIT = 40;

// WASM i64 values arrive as signed BigInt; checksums must be shown unsigned.
const u64 = (value: bigint): string => BigInt.asUintN(64, value).toString();

export class UltralobEngine {
  private readonly wasm: UltralobWasm;
  private readonly u64Ptr: number;
  private readonly f64Ptr: number;

  constructor(wasm: UltralobWasm) {
    this.wasm = wasm;
    // No growth (ALLOW_MEMORY_GROWTH=0), so these views stay valid.
    this.u64Ptr = wasm._ul_out_u64() >> 3;
    this.f64Ptr = wasm._ul_out_f64() >> 3;
    wasm._ul_init(1);
  }

  reset(): void {
    this.wasm._ul_init(1);
  }

  submitLimit(side: Side, priceTicks: number, qty: number): number {
    return this.wasm._ul_submit_limit(side, Math.round(priceTicks), Math.round(qty));
  }

  submitMarket(side: Side, qty: number): number {
    return this.wasm._ul_submit_market(side, Math.round(qty));
  }

  cancel(orderId: number): boolean {
    return this.wasm._ul_cancel(orderId) === 1;
  }

  modify(orderId: number, priceTicks: number, qty: number): boolean {
    return this.wasm._ul_modify(orderId, Math.round(priceTicks), Math.round(qty)) === 1;
  }

  hasOrder(orderId: number): boolean {
    return this.wasm._ul_has_order(orderId) === 1;
  }

  orderRemaining(orderId: number): number {
    return this.wasm._ul_order_remaining(orderId);
  }

  private readLevels(side: Side): Level[] {
    const count = this.wasm._ul_book_side(side, LEVEL_LIMIT);
    const levels: Level[] = [];
    for (let i = 0; i < count; i += 1) {
      levels.push({
        price: this.wasm.HEAPF64[this.f64Ptr + i],
        qty: Number(this.wasm.HEAPU64[this.u64Ptr + i * 2]),
        orders: Number(this.wasm.HEAPU64[this.u64Ptr + i * 2 + 1]),
      });
    }
    return levels;
  }

  private readTape(): TradeRow[] {
    const count = this.wasm._ul_trades(TAPE_LIMIT);
    const tape: TradeRow[] = [];
    for (let i = 0; i < count; i += 1) {
      tape.push({
        id: Number(this.wasm.HEAPU64[this.u64Ptr + i * 6]),
        price: this.wasm.HEAPF64[this.f64Ptr + i],
        qty: Number(this.wasm.HEAPU64[this.u64Ptr + i * 6 + 3]),
        takerSide: (Number(this.wasm.HEAPU64[this.u64Ptr + i * 6 + 5]) === 0 ? 0 : 1) as Side,
        seq: Number(this.wasm.HEAPU64[this.u64Ptr + i * 6 + 4]),
      });
    }
    return tape;
  }

  snapshot(): EngineState {
    const bids = this.readLevels(0);
    const asks = this.readLevels(1);
    const tape = this.readTape();
    return {
      seq: Number(this.wasm._ul_seq()),
      crc64: u64(this.wasm._ul_crc64()),
      activeOrders: Number(this.wasm._ul_active_orders()),
      trades: Number(this.wasm._ul_trade_count()),
      processed: Number(this.wasm._ul_processed()),
      rejected: Number(this.wasm._ul_rejected()),
      gaps: Number(this.wasm._ul_gaps()),
      duplicates: Number(this.wasm._ul_duplicates()),
      bestBid: this.wasm._ul_best_bid(),
      bestAsk: this.wasm._ul_best_ask(),
      spread: this.wasm._ul_spread(),
      bids,
      asks,
      tape,
      lastStatus: this.wasm._ul_last_status(),
      lastReject: this.wasm._ul_last_reject(),
      lastOrderId: Number(BigInt.asUintN(64, this.wasm._ul_last_order_id())),
    };
  }

  benchmark(events: number, seed: number, workload: Workload): BenchResult {
    this.wasm._ul_benchmark(events, seed, workload);
    const u = (index: number) => Number(this.wasm.HEAPU64[this.u64Ptr + index]);
    const f = (index: number) => this.wasm.HEAPF64[this.f64Ptr + index];
    return {
      events: u(0),
      trades: u(1),
      rejected: u(2),
      activeOrders: u(3),
      crc64: String(u(4)),
      elapsedNs: u(5),
      p50: u(6),
      p99: u(7),
      p999: u(8),
      p9999: u(9),
      max: u(10),
      mean: u(11),
      eventsPerSecond: f(0),
      matchingEventsPerSecond: f(1),
      workload,
      seed,
      ranAt: new Date().toISOString(),
    };
  }

  replayBegin(events: number, seed: number, workload: Workload, gapPolicy: GapPolicy): void {
    this.wasm._ul_replay_begin(events, seed, workload, gapPolicy);
  }

  replayStep(steps: number): number {
    return this.wasm._ul_replay_step(steps);
  }

  replayState(): ReplayState {
    const bids = (() => {
      const count = this.wasm._ul_replay_book_side(0, LEVEL_LIMIT);
      const levels: Level[] = [];
      for (let i = 0; i < count; i += 1) {
        levels.push({
          price: this.wasm.HEAPF64[this.f64Ptr + i],
          qty: Number(this.wasm.HEAPU64[this.u64Ptr + i * 2]),
          orders: Number(this.wasm.HEAPU64[this.u64Ptr + i * 2 + 1]),
        });
      }
      return levels;
    })();

    const asks = (() => {
      const count = this.wasm._ul_replay_book_side(1, LEVEL_LIMIT);
      const levels: Level[] = [];
      for (let i = 0; i < count; i += 1) {
        levels.push({
          price: this.wasm.HEAPF64[this.f64Ptr + i],
          qty: Number(this.wasm.HEAPU64[this.u64Ptr + i * 2]),
          orders: Number(this.wasm.HEAPU64[this.u64Ptr + i * 2 + 1]),
        });
      }
      return levels;
    })();

    let nextEvent = null;
    if (this.wasm._ul_replay_next_event() === 1) {
      nextEvent = {
        type: Number(this.wasm.HEAPU64[this.u64Ptr + 0]),
        side: Number(this.wasm.HEAPU64[this.u64Ptr + 1]) as Side,
        orderType: Number(this.wasm.HEAPU64[this.u64Ptr + 2]) as 0 | 1,
        orderId: Number(this.wasm.HEAPU64[this.u64Ptr + 3]),
        seq: Number(this.wasm.HEAPU64[this.u64Ptr + 4]),
        qty: Number(this.wasm.HEAPU64[this.u64Ptr + 5]),
        price: this.wasm.HEAPF64[this.f64Ptr + 0],
      };
    }

    return {
      generated: Number(this.wasm._ul_replay_generated()),
      crc64: u64(this.wasm._ul_replay_crc64()),
      trades: Number(this.wasm._ul_replay_trades()),
      gaps: this.wasm._ul_replay_gaps(),
      bids,
      asks,
      nextEvent,
    };
  }

  replayVerify(): VerifyResult {
    const equal = this.wasm._ul_replay_verify() === 1;
    return {
      equal,
      liveCrc: String(this.wasm.HEAPU64[this.u64Ptr + 0]),
      referenceCrc: String(this.wasm.HEAPU64[this.u64Ptr + 1]),
      seq: Number(this.wasm.HEAPU64[this.u64Ptr + 2]),
    };
  }

  chaos(scenario: number): ChaosReport {
    const pointer = this.wasm._ul_chaos_json(scenario);
    return JSON.parse(this.wasm.UTF8ToString(pointer)) as ChaosReport;
  }

  chaosAll(): ChaosReport[] {
    const pointer = this.wasm._ul_chaos_all_json();
    return JSON.parse(this.wasm.UTF8ToString(pointer)) as ChaosReport[];
  }
}
