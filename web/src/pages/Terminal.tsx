import { useEffect, useState } from 'react';
import { Panel, StatTile } from '../components/Panel';
import { OrderBookLadder } from '../components/OrderBookLadder';
import { OrderTicket } from '../components/OrderTicket';
import { TradeTape } from '../components/TradeTape';
import { formatPrice, formatQty } from '../lib/format';
import type { EngineClient } from '../hooks/useEngine';
import type { Side } from '../lib/types';

interface SessionOrder {
  id: number;
  side: Side;
  price: number;
  qty: number;
  remaining: number;
}

export function TerminalPage({ engine }: { engine: EngineClient }) {
  const state = engine.state;
  const [sessionOrders, setSessionOrders] = useState<SessionOrder[]>([]);
  const [seeding, setSeeding] = useState(false);
  const [quoted, setQuoted] = useState<{ ticks: number; nonce: number } | null>(null);

  const submitLimit = async (side: Side, price: number, qty: number) => {
    const next = await engine.submitLimit(side, price, qty);
    // Track the order only when the engine actually rested it, and use the
    // engine's remaining quantity: a limit order that crossed before resting
    // is already partially filled.
    const info = await engine.request<{ exists: boolean; remaining: number }>({
      type: 'hasOrder',
      orderId: next.lastOrderId,
    });
    if (next.lastOrderId > 0 && info.exists) {
      setSessionOrders((previous) => [
        {
          id: next.lastOrderId,
          side,
          price,
          qty,
          remaining: info.remaining,
        },
        ...previous.filter((order) => order.id !== next.lastOrderId),
      ]);
    } else if (next.lastOrderId > 0) {
      // Fully executed immediately (crossed the book): nothing to track.
      setSessionOrders((previous) => previous.filter((order) => order.id !== next.lastOrderId));
    }
  };

  const submitMarket = async (side: Side, qty: number) => {
    await engine.submitMarket(side, qty);
  };

  // Track resting orders submitted from this session and drop the ones the
  // engine filled or cancelled.
  useEffect(() => {
    if (!state || sessionOrders.length === 0) return;
    let cancelled = false;
    void (async () => {
      const refreshed = await Promise.all(
        sessionOrders.map(async (order) => {
          try {
            const info = await engine.request<{ exists: boolean; remaining: number }>({
              type: 'hasOrder',
              orderId: order.id,
            });
            return info.exists ? { ...order, remaining: info.remaining } : null;
          } catch {
            return order;
          }
        }),
      );
      if (!cancelled) {
        setSessionOrders(refreshed.filter((order): order is SessionOrder => order !== null));
      }
    })();
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state?.seq]);

  const seedBook = async () => {
    setSeeding(true);
    try {
      const mids = [10000];
      const mid = mids[0];
      for (let level = 0; level < 6; level += 1) {
        for (let slice = 0; slice < 2; slice += 1) {
          await engine.submitLimit(0, mid - level * 5, 30 + level * 15 + slice * 20);
          await engine.submitLimit(1, mid + 5 + level * 5, 30 + level * 15 + slice * 20);
        }
      }
    } finally {
      setSeeding(false);
      await engine.refresh();
    }
  };

  const mid = state && state.bestBid > 0 && state.bestAsk > 0 ? (state.bestBid + state.bestAsk) / 2 : 0;

  return (
    <div className="grid grid-cols-1 gap-4 xl:grid-cols-[330px_minmax(0,1fr)_380px]">
      <div className="flex flex-col gap-4">
        <Panel
          title="Order entry"
          hint="routes through the event processor"
          actions={
            <div className="flex gap-1.5">
              <button
                type="button"
                onClick={() => void seedBook()}
                disabled={seeding}
                className="rounded border border-line px-1.5 py-0.5 text-[10px] uppercase tracking-wider text-fog transition-colors hover:border-mint/40 hover:text-mint disabled:opacity-50"
              >
                seed book
              </button>
              <button
                type="button"
                onClick={() => {
                  void engine.reset();
                  setSessionOrders([]);
                }}
                className="rounded border border-line px-1.5 py-0.5 text-[10px] uppercase tracking-wider text-fog transition-colors hover:border-ask/40 hover:text-ask"
              >
                reset
              </button>
            </div>
          }
        >
          <OrderTicket
            bestBid={state?.bestBid ?? 0}
            bestAsk={state?.bestAsk ?? 0}
            lastStatus={state?.lastStatus ?? 0}
            lastReject={state?.lastReject ?? 0}
            disabled={state === null}
            quotedPrice={quoted}
            onSubmitLimit={submitLimit}
            onSubmitMarket={submitMarket}
          />
        </Panel>

        <Panel title="Engine" hint="live counters from C++ state">
          <div className="grid grid-cols-2 gap-2">
            <StatTile label="best bid" value={formatPrice(state?.bestBid ?? 0)} tone="bid" />
            <StatTile label="best ask" value={formatPrice(state?.bestAsk ?? 0)} tone="ask" />
            <StatTile
              label="spread"
              value={state && state.spread > 0 ? formatPrice(state.spread) : '—'}
            />
            <StatTile label="mid" value={mid > 0 ? formatPrice(mid) : '—'} />
            <StatTile label="resting orders" value={formatQty(state?.activeOrders ?? 0)} />
            <StatTile label="executions" value={formatQty(state?.trades ?? 0)} />
            <StatTile label="events" value={formatQty(state?.seq ?? 0)} sub={`${state?.processed ?? 0} applied`} />
            <StatTile
              label="rejected"
              value={formatQty(state?.rejected ?? 0)}
              tone={(state?.rejected ?? 0) > 0 ? 'amber' : 'default'}
            />
          </div>
        </Panel>
      </div>

      <Panel
        title="Order book"
        hint="price-time priority · click a level to quote it"
        bodyClassName="p-0"
        className="min-h-[520px]"
      >
        {state === null ? (
          <div className="flex h-full items-center justify-center text-[11px] text-fog/70">
            starting WASM engine…
          </div>
        ) : (
          <OrderBookLadder
            bids={state.bids}
            asks={state.asks}
            bestBid={state.bestBid}
            bestAsk={state.bestAsk}
            spread={state.spread}
            maxRows={12}
            highlightPrice={quoted?.ticks ?? null}
            onPriceSelect={(ticks) => setQuoted({ ticks, nonce: Date.now() })}
          />
        )}
      </Panel>

      <div className="flex flex-col gap-4">
        <Panel title="Trade tape" hint="newest first" bodyClassName="p-3" className="min-h-[280px]">
          <TradeTape tape={state?.tape ?? []} rows={16} />
        </Panel>

        <Panel
          title="Session orders"
          hint={`${sessionOrders.length} tracked`}
          bodyClassName="p-3"
        >
          {sessionOrders.length === 0 ? (
            <div className="py-3 text-center text-[11px] text-fog/60">
              rest a limit order to track it here
            </div>
          ) : (
            <div className="flex flex-col gap-1">
              {sessionOrders.map((order) => (
                <div
                  key={order.id}
                  className="grid grid-cols-[auto_1fr_auto_auto] items-center gap-2 rounded-md border border-line-soft bg-ink/50 px-2 py-1.5 text-[11px]"
                >
                  <span
                    className={`num text-[10px] uppercase ${order.side === 0 ? 'text-bid' : 'text-ask'}`}
                  >
                    {order.side === 0 ? 'buy' : 'sell'}
                  </span>
                  <span className="num text-mist">{formatPrice(order.price)}</span>
                  <span className="num text-right text-fog">{formatQty(order.remaining)}</span>
                  <button
                    type="button"
                    onClick={() => {
                      void engine.cancel(order.id).then(() => engine.refresh());
                    }}
                    className="rounded border border-line px-1.5 py-0.5 text-[9px] uppercase tracking-wider text-fog transition-colors hover:border-ask/40 hover:text-ask"
                  >
                    cancel
                  </button>
                </div>
              ))}
            </div>
          )}
        </Panel>
      </div>
    </div>
  );
}
