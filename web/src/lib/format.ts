export const TICK = 100; // 1 tick = 1 cent

export function priceToUnits(ticks: number): number {
  return ticks / TICK;
}

export function priceFromUnits(units: number): number {
  return Math.round(units * TICK);
}

export function formatPrice(ticks: number, digits = 2): string {
  if (!Number.isFinite(ticks) || ticks <= 0) return '—';
  return (ticks / TICK).toFixed(digits);
}

export function formatQty(qty: number): string {
  return qty.toLocaleString('en-US');
}

export function formatInt(value: number): string {
  return Math.round(value).toLocaleString('en-US');
}

export function compact(value: number): string {
  if (!Number.isFinite(value)) return '—';
  if (value >= 1e9) return `${(value / 1e9).toFixed(2)}B`;
  if (value >= 1e6) return `${(value / 1e6).toFixed(2)}M`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}k`;
  return value.toFixed(0);
}

// Latency samples of exactly zero mean the clock did not advance during the
// event, not that the work was free. The browser clock (performance.now() via
// Emscripten) is coarser than native CLOCK_MONOTONIC, so sub-resolution work
// reads 0. Saying so is more honest than printing "0 ns".
export function formatLatency(ns: number): string {
  if (ns <= 0) return '< 1 µs';
  return formatDuration(ns);
}

export function formatDuration(ns: number): string {
  if (ns >= 1e9) return `${(ns / 1e9).toFixed(2)} s`;
  if (ns >= 1e6) return `${(ns / 1e6).toFixed(2)} ms`;
  if (ns >= 1e3) return `${(ns / 1e3).toFixed(1)} µs`;
  return `${ns.toFixed(0)} ns`;
}

export function shortCrc(crc: string): string {
  if (crc.length <= 12) return crc;
  return `${crc.slice(0, 6)}…${crc.slice(-4)}`;
}

export const EVENT_TYPE_NAMES: Record<number, string> = {
  1: 'NEW_ORDER',
  2: 'CANCEL_ORDER',
  3: 'MODIFY_ORDER',
  4: 'TRADE_EXECUTED',
  5: 'SNAPSHOT_CREATED',
  6: 'GAP_DETECTED',
  7: 'RECOVERY_COMPLETED',
};

export const STATUS_NAMES: Record<number, string> = {
  0: 'accepted',
  1: 'rejected',
  2: 'duplicate',
  3: 'gap',
  4: 'out_of_order',
  5: 'buffered',
  6: 'halted',
};

export const REJECT_NAMES: Record<number, string> = {
  0: 'none',
  1: 'invalid quantity',
  2: 'invalid price',
  3: 'duplicate order id',
  4: 'unknown order',
  5: 'order not resting',
  6: 'book full',
  7: 'order pool exhausted',
  8: 'index full',
};
