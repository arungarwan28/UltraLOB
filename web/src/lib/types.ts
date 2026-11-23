// Shared types between the UI thread and the engine Web Worker.

export type Side = 0 | 1; // 0 = buy/bid, 1 = sell/ask
export type OrderType = 0 | 1; // 0 = limit, 1 = market
export type Workload = 0 | 1 | 2; // balanced, cancel_heavy, burst
export type GapPolicy = 0 | 1 | 2; // halt, continue, buffer

export interface Level {
  price: number; // ticks
  qty: number;
  orders: number;
}

export interface TradeRow {
  id: number;
  price: number;
  qty: number;
  takerSide: Side;
  seq: number;
}

export interface EngineState {
  seq: number;
  crc64: string;
  activeOrders: number;
  trades: number;
  processed: number;
  rejected: number;
  gaps: number;
  duplicates: number;
  bestBid: number;
  bestAsk: number;
  spread: number;
  bids: Level[];
  asks: Level[];
  tape: TradeRow[];
  lastStatus: number;
  lastReject: number;
  lastOrderId: number;
}

export interface BenchResult {
  events: number;
  trades: number;
  rejected: number;
  activeOrders: number;
  crc64: string;
  elapsedNs: number;
  p50: number;
  p99: number;
  p999: number;
  p9999: number;
  max: number;
  mean: number;
  eventsPerSecond: number;
  matchingEventsPerSecond: number;
  workload: Workload;
  seed: number;
  ranAt: string;
}

export interface ReplayEvent {
  type: number;
  side: Side;
  orderType: OrderType;
  orderId: number;
  seq: number;
  qty: number;
  price: number;
}

export interface ReplayState {
  generated: number;
  crc64: string;
  trades: number;
  gaps: number;
  bids: Level[];
  asks: Level[];
  nextEvent: ReplayEvent | null;
}

export interface VerifyResult {
  equal: boolean;
  liveCrc: string;
  referenceCrc: string;
  seq: number;
}

export interface ChaosReport {
  scenario: string;
  title: string;
  description: string;
  passed: boolean;
  detected: number;
  applied: number;
  rejected: number;
  duplicates: number;
  gaps: number;
  out_of_order: number;
  buffered: number;
  drained: number;
  dropped: number;
  retries: number;
  crc_baseline: string;
  crc_observed: string;
  state_matches: boolean;
  detail: string;
}

export interface NativeRun {
  workload: string;
  events: number;
  seed: number;
  repeat: number;
  elapsed_ms: number;
  events_per_second: number;
  matching_events_per_second: number;
  trades: number;
  rejected: number;
  latency_ns: {
    p50: number;
    p99: number;
    p99_9: number;
    p99_99: number;
    max: number;
    mean: number;
  };
  heap_allocations: number;
  allocated_bytes: number;
  peak_rss_kb: number;
  active_orders: number;
  crc64: string;
}

export interface NativeBenchmark {
  generated_at: string;
  environment: Record<string, string | number>;
  engine_limits: Record<string, number>;
  runs: NativeRun[];
  components: { name: string; ops: number; ns_per_op: number }[];
}
