import { useEffect, useState } from 'react';
import { cx } from '../lib/cx';
import { priceFromUnits, formatPrice } from '../lib/format';
import { REJECT_NAMES, STATUS_NAMES } from '../lib/format';
import type { Side } from '../lib/types';

interface OrderTicketProps {
  bestBid: number;
  bestAsk: number;
  lastStatus: number;
  lastReject: number;
  disabled?: boolean;
  // A price picked from the book ladder. The nonce makes repeated clicks on the
  // same level re-apply the quote instead of being swallowed by state equality.
  quotedPrice?: { ticks: number; nonce: number } | null;
  onSubmitLimit: (side: Side, priceTicks: number, qty: number) => Promise<void>;
  onSubmitMarket: (side: Side, qty: number) => Promise<void>;
}

const QUICK_QTY = [10, 25, 50, 100];

export function OrderTicket({
  bestBid,
  bestAsk,
  lastStatus,
  lastReject,
  disabled,
  quotedPrice,
  onSubmitLimit,
  onSubmitMarket,
}: OrderTicketProps) {
  const [side, setSide] = useState<Side>(0);
  const [type, setType] = useState<'limit' | 'market'>('limit');
  const [price, setPrice] = useState('');
  const [qty, setQty] = useState('25');
  const [touched, setTouched] = useState(false);
  // True when the price came from the ladder: switching side must not throw it
  // away and re-quote the touch (that made the picked price silently change).
  const [pinned, setPinned] = useState(false);
  const [localError, setLocalError] = useState<string | null>(null);

  // Track the touch until the user types a price themselves.
  useEffect(() => {
    if (touched) return;
    const reference = side === 0 ? bestBid || bestAsk - 100 : bestAsk || bestBid + 100;
    if (reference > 0) setPrice(formatPrice(reference));
  }, [side, bestBid, bestAsk, touched]);

  // Quoting from the ladder is an explicit price choice: switch back to a limit
  // order and stop following the touch.
  useEffect(() => {
    if (!quotedPrice || quotedPrice.ticks <= 0) return;
    setType('limit');
    setTouched(true);
    setPinned(true);
    setLocalError(null);
    setPrice(formatPrice(quotedPrice.ticks));
  }, [quotedPrice]);

  const submit = async () => {
    setLocalError(null);
    const quantity = Number(qty);
    if (!Number.isFinite(quantity) || quantity <= 0) {
      setLocalError('quantity must be a positive integer');
      return;
    }
    if (type === 'market') {
      await onSubmitMarket(side, Math.floor(quantity));
      return;
    }
    const ticks = priceFromUnits(Number(price));
    if (!Number.isFinite(ticks) || ticks <= 0) {
      setLocalError('price must be a positive amount');
      return;
    }
    await onSubmitLimit(side, ticks, Math.floor(quantity));
  };

  const rejectMessage =
    lastStatus === 1 ? `engine rejected: ${REJECT_NAMES[lastReject] ?? `code ${lastReject}`}` : null;

  return (
    <div className="flex flex-col gap-3">
      <div className="grid grid-cols-2 gap-2">
        <div className="grid grid-cols-2 gap-1 rounded-lg border border-line bg-ink p-1">
          <button
            type="button"
            onClick={() => {
              setSide(0);
              if (!pinned) setTouched(false);
            }}
            className={cx(
              'rounded-md px-2 py-1.5 text-[11px] font-semibold uppercase tracking-[0.12em] transition-colors',
              side === 0 ? 'bg-bid/15 text-bid' : 'text-fog hover:text-mist',
            )}
          >
            buy
          </button>
          <button
            type="button"
            onClick={() => {
              setSide(1);
              if (!pinned) setTouched(false);
            }}
            className={cx(
              'rounded-md px-2 py-1.5 text-[11px] font-semibold uppercase tracking-[0.12em] transition-colors',
              side === 1 ? 'bg-ask/15 text-ask' : 'text-fog hover:text-mist',
            )}
          >
            sell
          </button>
        </div>
        <div className="grid grid-cols-2 gap-1 rounded-lg border border-line bg-ink p-1">
          {(['limit', 'market'] as const).map((option) => (
            <button
              key={option}
              type="button"
              onClick={() => setType(option)}
              className={cx(
                'rounded-md px-2 py-1.5 text-[11px] font-semibold uppercase tracking-[0.12em] transition-colors',
                type === option ? 'bg-panel-2 text-chalk' : 'text-fog hover:text-mist',
              )}
            >
              {option}
            </button>
          ))}
        </div>
      </div>

      <label className="flex flex-col gap-1">
        <span className="text-[10px] uppercase tracking-[0.14em] text-fog">price (USD)</span>
        <input
          value={type === 'market' ? '' : price}
          disabled={type === 'market'}
          onChange={(event) => {
            setTouched(true);
            setPinned(false);
            setPrice(event.target.value);
          }}
          inputMode="decimal"
          placeholder={type === 'market' ? 'at best available' : '0.00'}
          className={cx(
            'num rounded-lg border border-line bg-ink px-3 py-2 text-sm text-chalk outline-none transition-colors',
            'placeholder:text-fog/50 focus:border-mint/60 disabled:text-fog/40',
          )}
        />
      </label>

      <label className="flex flex-col gap-1">
        <span className="text-[10px] uppercase tracking-[0.14em] text-fog">quantity</span>
        <div className="flex items-center gap-2">
          <input
            value={qty}
            onChange={(event) => setQty(event.target.value)}
            inputMode="numeric"
            className="num w-full rounded-lg border border-line bg-ink px-3 py-2 text-sm text-chalk outline-none transition-colors focus:border-mint/60"
          />
          <div className="flex gap-1">
            {QUICK_QTY.map((value) => (
              <button
                key={value}
                type="button"
                onClick={() => setQty(String(value))}
                className="rounded-md border border-line px-1.5 py-1 text-[10px] text-fog transition-colors hover:border-mint/40 hover:text-mint"
              >
                {value}
              </button>
            ))}
          </div>
        </div>
      </label>

      <button
        type="button"
        onClick={() => void submit()}
        disabled={disabled}
        className={cx(
          'mt-1 rounded-lg px-3 py-2.5 text-[12px] font-semibold uppercase tracking-[0.16em] transition-transform active:translate-y-px',
          side === 0
            ? 'bg-bid/20 text-bid ring-1 ring-bid/40 hover:bg-bid/25'
            : 'bg-ask/20 text-ask ring-1 ring-ask/40 hover:bg-ask/25',
          disabled && 'cursor-not-allowed opacity-50',
        )}
      >
        {type === 'limit' ? 'submit limit' : 'submit market'} {side === 0 ? 'buy' : 'sell'}
      </button>

      <div className="min-h-[16px] text-[11px] leading-snug">
        {(localError ?? rejectMessage) && (
          <span className="text-amber">{localError ?? rejectMessage}</span>
        )}
        {!localError && !rejectMessage && lastStatus === 0 && (
          <span className="text-fog/70">
            every submission is an event: NEW_ORDER → matching engine → TRADE_EXECUTED
          </span>
        )}
        {!localError && !rejectMessage && lastStatus !== 0 && (
          <span className="text-fog/70">last event: {STATUS_NAMES[lastStatus] ?? lastStatus}</span>
        )}
      </div>
    </div>
  );
}
