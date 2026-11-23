import { useMemo, useState } from 'react';
import { Panel, StatTile } from '../components/Panel';
import { DepthChart } from '../components/DepthChart';
import { OrderBookLadder } from '../components/OrderBookLadder';
import { formatPrice, formatQty } from '../lib/format';
import type { EngineClient } from '../hooks/useEngine';
import type { Side } from '../lib/types';

export function BookPage({ engine }: { engine: EngineClient }) {
  const state = engine.state;
  const [probeSide, setProbeSide] = useState<Side>(0);
  const [probeQty, setProbeQty] = useState('250');
  const [impact, setImpact] = useState<{
    filled: number;
    averagePrice: number;
    slippage: number;
    orders: number;
  } | null>(null);

  const totals = useMemo(() => {
    const bidQty = (state?.bids ?? []).reduce((sum, level) => sum + level.qty, 0);
    const askQty = (state?.asks ?? []).reduce((sum, level) => sum + level.qty, 0);
    const total = bidQty + askQty;
    return {
      bidQty,
      askQty,
      imbalance: total === 0 ? 0 : bidQty / total,
    };
  }, [state]);

  const runProbe = async () => {
    if (state === null) return;
    const quantity = Math.max(1, Math.floor(Number(probeQty)));
    const before = state.tape[0]?.id ?? 0;
    const referencePrice = probeSide === 0 ? state.bestAsk : state.bestBid;
    const next = await engine.submitMarket(probeSide, quantity);

    const newTrades = next.tape.filter((trade) => trade.id > before);
    const filled = newTrades.reduce((sum, trade) => sum + trade.qty, 0);
    const notional = newTrades.reduce((sum, trade) => sum + trade.price * trade.qty, 0);
    const averagePrice = filled === 0 ? 0 : notional / filled;
    setImpact({
      filled,
      averagePrice,
      slippage: referencePrice > 0 && filled > 0 ? averagePrice - referencePrice : 0,
      orders: newTrades.length,
    });
    await engine.refresh();
  };

  return (
    <div className="grid grid-cols-1 gap-4 xl:grid-cols-[minmax(0,1fr)_420px]">
      <div className="flex flex-col gap-4">
        <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
          <StatTile label="best bid" value={formatPrice(state?.bestBid ?? 0)} tone="bid" />
          <StatTile label="best ask" value={formatPrice(state?.bestAsk ?? 0)} tone="ask" />
          <StatTile
            label="spread"
            value={state && state.spread > 0 ? formatPrice(state.spread) : '—'}
            sub={state && state.bestBid > 0 && state.bestAsk > 0 ? `${state.spread} ticks` : undefined}
          />
          <StatTile
            label="bid / ask imbalance"
            value={`${(totals.imbalance * 100).toFixed(1)}%`}
            sub={`${formatQty(totals.bidQty)} bid · ${formatQty(totals.askQty)} ask`}
            tone={totals.imbalance >= 0.5 ? 'bid' : 'ask'}
          />
        </div>

        <Panel title="Market depth" hint="cumulative quantity by price (simulated instrument)">
          <DepthChart bids={state?.bids ?? []} asks={state?.asks ?? []} height={300} />
        </Panel>

        <Panel title="Liquidity by level" bodyClassName="p-0">
          <div className="max-h-[340px] overflow-auto">
            <table className="w-full text-xs">
              <thead className="sticky top-0 bg-panel/95 text-[10px] uppercase tracking-[0.12em] text-fog/70">
                <tr>
                  <th className="px-4 py-2 text-left font-medium">side</th>
                  <th className="px-4 py-2 text-right font-medium">price</th>
                  <th className="px-4 py-2 text-right font-medium">quantity</th>
                  <th className="px-4 py-2 text-right font-medium">orders</th>
                  <th className="px-4 py-2 text-right font-medium">distance</th>
                </tr>
              </thead>
              <tbody className="num">
                {(state?.asks ?? []).slice(0, 8).map((level) => (
                  <tr key={`ask-${level.price}`} className="border-t border-line-soft/60">
                    <td className="px-4 py-1.5 text-ask">ask</td>
                    <td className="px-4 py-1.5 text-right text-mist">{formatPrice(level.price)}</td>
                    <td className="px-4 py-1.5 text-right text-chalk">{formatQty(level.qty)}</td>
                    <td className="px-4 py-1.5 text-right text-fog">{level.orders}</td>
                    <td className="px-4 py-1.5 text-right text-fog">
                      {state && state.bestAsk > 0 ? `+${level.price - state.bestAsk}` : '—'}
                    </td>
                  </tr>
                ))}
                {(state?.bids ?? []).slice(0, 8).map((level) => (
                  <tr key={`bid-${level.price}`} className="border-t border-line-soft/60">
                    <td className="px-4 py-1.5 text-bid">bid</td>
                    <td className="px-4 py-1.5 text-right text-mist">{formatPrice(level.price)}</td>
                    <td className="px-4 py-1.5 text-right text-chalk">{formatQty(level.qty)}</td>
                    <td className="px-4 py-1.5 text-right text-fog">{level.orders}</td>
                    <td className="px-4 py-1.5 text-right text-fog">
                      {state && state.bestBid > 0 ? `-${state.bestBid - level.price}` : '—'}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Panel>
      </div>

      <div className="flex flex-col gap-4">
        <Panel title="Book ladder" bodyClassName="p-0" className="min-h-[460px]">
          {state && (
            <OrderBookLadder
              bids={state.bids}
              asks={state.asks}
              bestBid={state.bestBid}
              bestAsk={state.bestAsk}
              spread={state.spread}
              maxRows={8}
            />
          )}
        </Panel>

        <Panel title="Impact probe" hint="market order against live depth">
          <div className="flex flex-col gap-3">
            <div className="grid grid-cols-2 gap-1 rounded-lg border border-line bg-ink p-1">
              <button
                type="button"
                onClick={() => setProbeSide(0)}
                className={`rounded-md px-2 py-1.5 text-[11px] font-semibold uppercase tracking-[0.12em] ${
                  probeSide === 0 ? 'bg-bid/15 text-bid' : 'text-fog hover:text-mist'
                }`}
              >
                buy
              </button>
              <button
                type="button"
                onClick={() => setProbeSide(1)}
                className={`rounded-md px-2 py-1.5 text-[11px] font-semibold uppercase tracking-[0.12em] ${
                  probeSide === 1 ? 'bg-ask/15 text-ask' : 'text-fog hover:text-mist'
                }`}
              >
                sell
              </button>
            </div>
            <input
              value={probeQty}
              onChange={(event) => setProbeQty(event.target.value)}
              inputMode="numeric"
              className="num rounded-lg border border-line bg-ink px-3 py-2 text-sm text-chalk outline-none focus:border-mint/60"
            />
            <button
              type="button"
              onClick={() => void runProbe()}
              className="rounded-lg bg-panel-2 px-3 py-2 text-[11px] font-semibold uppercase tracking-[0.14em] text-chalk ring-1 ring-line transition-colors hover:ring-mint/50"
            >
              execute probe
            </button>

            {impact && (
              <div className="grid grid-cols-2 gap-2">
                <StatTile label="filled" value={formatQty(impact.filled)} />
                <StatTile label="fills" value={formatQty(impact.orders)} />
                <StatTile
                  label="average price"
                  value={impact.averagePrice > 0 ? formatPrice(impact.averagePrice) : '—'}
                />
                <StatTile
                  label="slippage"
                  value={
                    impact.filled === 0
                      ? '—'
                      : `${impact.slippage >= 0 ? '+' : ''}${formatPrice(Math.abs(impact.slippage))}`
                  }
                  tone={impact.slippage > 0 ? 'ask' : 'bid'}
                  sub={impact.filled === 0 ? 'no liquidity' : `${impact.slippage} ticks vs touch`}
                />
              </div>
            )}
            <p className="text-[10px] leading-relaxed text-fog/70">
              A market order consumes the best prices first and never rests: the unfilled
              remainder is cancelled (IOC), which is the engine's documented policy.
            </p>
          </div>
        </Panel>
      </div>
    </div>
  );
}
