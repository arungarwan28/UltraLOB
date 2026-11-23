import { useMemo } from 'react';
import { cx } from '../lib/cx';
import { formatPrice, formatQty } from '../lib/format';
import type { Level } from '../lib/types';

interface LadderProps {
  bids: Level[];
  asks: Level[];
  bestBid: number;
  bestAsk: number;
  spread: number;
  maxRows?: number;
  showOrders?: boolean;
  onPriceSelect?: (ticks: number) => void;
  highlightPrice?: number | null;
}

export function OrderBookLadder({
  bids,
  asks,
  bestBid,
  bestAsk,
  spread,
  maxRows = 10,
  showOrders = true,
  onPriceSelect,
  highlightPrice = null,
}: LadderProps) {
  const visibleAsks = useMemo(() => asks.slice(0, maxRows), [asks, maxRows]);
  const visibleBids = useMemo(() => bids.slice(0, maxRows), [bids, maxRows]);

  const scale = useMemo(() => {
    const totals = [...visibleAsks, ...visibleBids].map((level) => level.qty);
    return totals.length === 0 ? 1 : Math.max(...totals);
  }, [visibleAsks, visibleBids]);

  const row = (level: Level, side: 'bid' | 'ask') => {
    const share = Math.max(2, Math.round((level.qty / scale) * 100));
    const isHighlighted = highlightPrice !== null && level.price === highlightPrice;
    return (
      <button
        key={`${side}-${level.price}`}
        type="button"
        onClick={() => onPriceSelect?.(level.price)}
        className={cx(
          'group relative grid w-full grid-cols-[1fr_auto_auto] items-center gap-3 px-3 py-[3px] text-left text-xs transition-colors',
          onPriceSelect ? 'hover:bg-panel-2/80' : 'cursor-default',
          isHighlighted && 'bg-mint/10',
        )}
      >
        <span
          aria-hidden
          className={cx(
            'absolute inset-y-[1px] right-0 rounded-l-sm',
            side === 'bid' ? 'bg-bid/10' : 'bg-ask/10',
          )}
          style={{ width: `${share}%` }}
        />
        <span
          className={cx(
            'num relative z-10 font-medium',
            side === 'bid' ? 'text-bid' : 'text-ask',
          )}
        >
          {formatPrice(level.price)}
        </span>
        <span className="num relative z-10 text-right text-mist">{formatQty(level.qty)}</span>
        {showOrders && (
          <span className="num relative z-10 text-right text-[10px] text-fog/80">
            {level.orders}
          </span>
        )}
      </button>
    );
  };

  return (
    <div className="flex min-h-0 flex-col">
      <div
        className={cx(
          'grid grid-cols-[1fr_auto_auto] gap-3 border-b border-line-soft px-3 pb-1.5 text-[10px] uppercase tracking-[0.12em] text-fog/70',
          !showOrders && 'grid-cols-[1fr_auto]',
        )}
      >
        <span>price</span>
        <span className="text-right">size</span>
        {showOrders && <span className="text-right">orders</span>}
      </div>

      <div className="flex flex-col-reverse py-1">
        {visibleAsks.length === 0 && (
          <div className="px-3 py-1 text-[11px] text-fog/60">no offers</div>
        )}
        {visibleAsks.map((level) => row(level, 'ask'))}
      </div>

      <div className="flex items-center justify-between border-y border-line-soft bg-ink/70 px-3 py-1.5">
        <span className="text-[10px] uppercase tracking-[0.14em] text-fog">spread</span>
        <span className="num text-[11px] text-chalk">
          {spread > 0 ? formatPrice(spread) : '—'}
          <span className="ml-2 text-fog/70">
            {bestBid > 0 ? formatPrice(bestBid) : '—'} / {bestAsk > 0 ? formatPrice(bestAsk) : '—'}
          </span>
        </span>
      </div>

      <div className="flex flex-col py-1">
        {visibleBids.length === 0 && (
          <div className="px-3 py-1 text-[11px] text-fog/60">no bids</div>
        )}
        {visibleBids.map((level) => row(level, 'bid'))}
      </div>
    </div>
  );
}
