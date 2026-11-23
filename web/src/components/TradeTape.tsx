import { cx } from '../lib/cx';
import { formatPrice, formatQty } from '../lib/format';
import type { TradeRow } from '../lib/types';

export function TradeTape({ tape, rows = 14 }: { tape: TradeRow[]; rows?: number }) {
  if (tape.length === 0) {
    return (
      <div className="flex h-full items-center justify-center text-[11px] text-fog/60">
        no executions yet — submit a crossing order
      </div>
    );
  }

  return (
    <div className="flex flex-col">
      <div className="grid grid-cols-[auto_1fr_auto_auto] gap-3 border-b border-line-soft pb-1.5 text-[10px] uppercase tracking-[0.12em] text-fog/70">
        <span>seq</span>
        <span>price</span>
        <span className="text-right">size</span>
        <span className="text-right">taker</span>
      </div>
      <div className="min-h-0 overflow-y-auto">
        {tape.slice(0, rows).map((trade) => (
          <div
            key={trade.id}
            className="grid grid-cols-[auto_1fr_auto_auto] items-center gap-3 border-b border-line-soft/40 py-[3px] text-xs last:border-0"
          >
            <span className="num text-[10px] text-fog/70">#{trade.seq}</span>
            <span className="num text-mist">{formatPrice(trade.price)}</span>
            <span className="num text-right text-chalk">{formatQty(trade.qty)}</span>
            <span
              className={cx(
                'num text-right text-[10px] uppercase tracking-wider',
                trade.takerSide === 0 ? 'text-bid' : 'text-ask',
              )}
            >
              {trade.takerSide === 0 ? 'buy' : 'sell'}
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}
