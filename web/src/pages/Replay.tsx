import { useEffect, useState } from 'react';
import { Badge, Panel, StatTile } from '../components/Panel';
import { OrderBookLadder } from '../components/OrderBookLadder';
import { EVENT_TYPE_NAMES, formatInt, formatPrice, formatQty, shortCrc } from '../lib/format';
import type { EngineClient } from '../hooks/useEngine';
import type { GapPolicy, ReplayState, VerifyResult, Workload } from '../lib/types';

const WORKLOADS: { value: Workload; label: string }[] = [
  { value: 0, label: 'balanced' },
  { value: 1, label: 'cancel heavy' },
  { value: 2, label: 'burst' },
];

const GAP_POLICIES: { value: GapPolicy; label: string }[] = [
  { value: 2, label: 'buffer' },
  { value: 1, label: 'reject + continue' },
  { value: 0, label: 'reject + halt' },
];

export function ReplayPage({ engine }: { engine: EngineClient }) {
  const [events, setEvents] = useState(20_000);
  const [seed, setSeed] = useState('99');
  const [workload, setWorkload] = useState<Workload>(0);
  const [gapPolicy, setGapPolicy] = useState<GapPolicy>(2);
  const [state, setState] = useState<ReplayState | null>(null);
  const [verification, setVerification] = useState<VerifyResult | null>(null);
  const [busy, setBusy] = useState(false);

  const begin = async () => {
    setBusy(true);
    setVerification(null);
    try {
      const next = await engine.request<ReplayState>({
        type: 'replayBegin',
        events,
        seed: Number(seed) || 1,
        workload,
        gapPolicy,
      });
      setState(next);
    } finally {
      setBusy(false);
    }
  };

  useEffect(() => {
    void begin();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const step = async (steps: number) => {
    const next = await engine.request<ReplayState>({ type: 'replayStep', steps });
    setState(next);
  };

  const verify = async () => {
    const result = await engine.request<VerifyResult>({ type: 'replayVerify' });
    setVerification(result);
  };

  const progress = state ? Math.min(100, (state.generated / Math.max(1, events)) * 100) : 0;

  return (
    <div className="grid grid-cols-1 gap-4 xl:grid-cols-[360px_minmax(0,1fr)_400px]">
      <div className="flex flex-col gap-4">
        <Panel title="Stream configuration" hint="deterministic feed generator">
          <div className="flex flex-col gap-3">
            <label className="flex flex-col gap-1">
              <span className="text-[10px] uppercase tracking-[0.14em] text-fog">events</span>
              <input
                value={events}
                onChange={(event) => setEvents(Number(event.target.value) || 0)}
                inputMode="numeric"
                className="num rounded-lg border border-line bg-ink px-3 py-2 text-sm text-chalk outline-none focus:border-mint/60"
              />
            </label>
            <label className="flex flex-col gap-1">
              <span className="text-[10px] uppercase tracking-[0.14em] text-fog">seed</span>
              <input
                value={seed}
                onChange={(event) => setSeed(event.target.value)}
                inputMode="numeric"
                className="num rounded-lg border border-line bg-ink px-3 py-2 text-sm text-chalk outline-none focus:border-mint/60"
              />
            </label>
            <div className="flex flex-col gap-1">
              <span className="text-[10px] uppercase tracking-[0.14em] text-fog">workload</span>
              <div className="grid grid-cols-3 gap-1 rounded-lg border border-line bg-ink p-1">
                {WORKLOADS.map((option) => (
                  <button
                    key={option.value}
                    type="button"
                    onClick={() => setWorkload(option.value)}
                    className={`rounded-md px-1 py-1.5 text-[10px] font-semibold uppercase tracking-[0.08em] ${
                      workload === option.value ? 'bg-panel-2 text-mint' : 'text-fog hover:text-mist'
                    }`}
                  >
                    {option.label}
                  </button>
                ))}
              </div>
            </div>
            <div className="flex flex-col gap-1">
              <span className="text-[10px] uppercase tracking-[0.14em] text-fog">gap policy</span>
              <div className="grid grid-cols-3 gap-1 rounded-lg border border-line bg-ink p-1">
                {GAP_POLICIES.map((option) => (
                  <button
                    key={option.value}
                    type="button"
                    onClick={() => setGapPolicy(option.value)}
                    className={`rounded-md px-1 py-1.5 text-[9px] font-semibold uppercase tracking-[0.05em] ${
                      gapPolicy === option.value ? 'bg-panel-2 text-chalk' : 'text-fog hover:text-mist'
                    }`}
                  >
                    {option.label}
                  </button>
                ))}
              </div>
            </div>
            <button
              type="button"
              onClick={() => void begin()}
              disabled={busy}
              className="rounded-lg bg-mint/15 px-3 py-2.5 text-[11px] font-semibold uppercase tracking-[0.16em] text-mint ring-1 ring-mint/40 transition-colors hover:bg-mint/20 disabled:opacity-60"
            >
              load stream
            </button>
          </div>
        </Panel>

        <Panel title="Transport controls">
          <div className="grid grid-cols-2 gap-2">
            {[
              { label: 'step 1', steps: 1 },
              { label: 'step 100', steps: 100 },
              { label: 'step 1k', steps: 1000 },
              { label: 'finish', steps: Math.max(0, events - (state?.generated ?? 0)) },
            ].map((control) => (
              <button
                key={control.label}
                type="button"
                onClick={() => void step(control.steps)}
                className="rounded-lg border border-line bg-ink px-2 py-2 text-[10px] font-semibold uppercase tracking-[0.12em] text-mist transition-colors hover:border-mint/40 hover:text-mint"
              >
                {control.label}
              </button>
            ))}
          </div>
          <button
            type="button"
            onClick={() => void verify()}
            className="mt-2 w-full rounded-lg bg-panel-2 px-3 py-2 text-[10px] font-semibold uppercase tracking-[0.14em] text-chalk ring-1 ring-line transition-colors hover:ring-mint/50"
          >
            compare checksums (independent second run)
          </button>

          <div className="mt-3">
            <div className="mb-1 flex items-center justify-between text-[10px] uppercase tracking-[0.12em] text-fog">
              <span>progress</span>
              <span className="num">
                {formatInt(state?.generated ?? 0)} / {formatInt(events)}
              </span>
            </div>
            <div className="h-1.5 w-full overflow-hidden rounded-full bg-ink">
              <div
                className="h-full rounded-full bg-mint/70 transition-[width] duration-150"
                style={{ width: `${progress}%` }}
              />
            </div>
          </div>
        </Panel>
      </div>

      <div className="flex flex-col gap-4">
        <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
          <StatTile label="events applied" value={formatInt(state?.generated ?? 0)} />
          <StatTile label="executions" value={formatInt(state?.trades ?? 0)} />
          <StatTile
            label="gaps detected"
            value={formatInt(state?.gaps ?? 0)}
            tone={(state?.gaps ?? 0) > 0 ? 'amber' : 'default'}
          />
          <StatTile label="state checksum" value={state ? shortCrc(state.crc64) : '—'} sub="CRC64 over book state" />
        </div>

        <Panel title="Next generated event" hint="peeked without consuming">
          {state?.nextEvent ? (
            <div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
              <StatTile label="sequence" value={`#${formatInt(state.nextEvent.seq)}`} />
              <StatTile
                label="event"
                value={
                  <span className="text-[12px]">
                    {EVENT_TYPE_NAMES[state.nextEvent.type] ?? state.nextEvent.type}
                  </span>
                }
              />
              <StatTile
                label="side / type"
                value={
                  <span className="text-[12px]">
                    {state.nextEvent.side === 0 ? 'buy' : 'sell'} ·{' '}
                    {state.nextEvent.orderType === 1 ? 'market' : 'limit'}
                  </span>
                }
              />
              <StatTile
                label="price / qty"
                value={
                  <span className="text-[12px]">
                    {state.nextEvent.price > 0 ? formatPrice(state.nextEvent.price) : 'mkt'} ·{' '}
                    {formatQty(state.nextEvent.qty)}
                  </span>
                }
              />
            </div>
          ) : (
            <div className="py-2 text-[11px] text-fog/60">stream not loaded</div>
          )}
        </Panel>

        <Panel title="Book at checkpoint" bodyClassName="p-0" className="min-h-[380px]">
          {state && (
            <OrderBookLadder
              bids={state.bids}
              asks={state.asks}
              bestBid={state.bids[0]?.price ?? 0}
              bestAsk={state.asks[0]?.price ?? 0}
              spread={
                state.bids[0] && state.asks[0] ? state.asks[0].price - state.bids[0].price : -1
              }
              maxRows={8}
            />
          )}
        </Panel>
      </div>

      <div className="flex flex-col gap-4">
        <Panel title="Checksum comparison">
          {verification === null ? (
            <p className="text-[11px] leading-relaxed text-fog/70">
              Run the stream, then compare checksums. The comparison replays the same feed
              through a second, independent engine instance and compares the CRC64 of the two
              books — it is a real cross-run equality check, not a cached value.
            </p>
          ) : (
            <div className="flex flex-col gap-3">
              <div className="flex items-center gap-2">
                <Badge tone={verification.equal ? 'mint' : 'ask'}>
                  {verification.equal ? 'identical state' : 'state diverged'}
                </Badge>
                <span className="num text-[11px] text-fog">
                  at sequence #{formatInt(verification.seq)}
                </span>
              </div>
              <div className="grid gap-2">
                <div className="rounded-lg border border-line-soft bg-ink/60 px-3 py-2">
                  <div className="text-[10px] uppercase tracking-[0.12em] text-fog">live run</div>
                  <div className="num mt-0.5 text-[11px] break-all text-mint">
                    {verification.liveCrc}
                  </div>
                </div>
                <div className="rounded-lg border border-line-soft bg-ink/60 px-3 py-2">
                  <div className="text-[10px] uppercase tracking-[0.12em] text-fog">
                    independent reference run
                  </div>
                  <div className="num mt-0.5 text-[11px] break-all text-mist">
                    {verification.referenceCrc}
                  </div>
                </div>
              </div>
            </div>
          )}
        </Panel>

        <Panel title="What this proves">
          <ul className="flex flex-col gap-2 text-[11px] leading-relaxed text-fog">
            <li>
              <span className="text-mist">Determinism.</span> The same (seed, workload, event
              count) always produces the same final book, byte for byte.
            </li>
            <li>
              <span className="text-mist">Ordering.</span> Events are applied exactly once, in
              sequence order, through the event processor — never straight into the book.
            </li>
            <li>
              <span className="text-mist">Anomalies are visible.</span> Gaps, duplicates and
              out-of-order arrivals are counted and reported, not silently repaired.
            </li>
            <li>
              <span className="text-mist">Checkpointing.</span> A serialized book image restores
              to the same checksum; the reliability lab exercises it.
            </li>
          </ul>
        </Panel>
      </div>
    </div>
  );
}
