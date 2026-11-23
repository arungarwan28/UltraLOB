import type { ReactNode } from 'react';
import { cx } from '../lib/cx';
import { shortCrc } from '../lib/format';
import type { EngineState } from '../lib/types';

export type PageKey =
  | 'terminal'
  | 'book'
  | 'performance'
  | 'replay'
  | 'reliability'
  | 'architecture';

export const NAV: { key: PageKey; label: string; index: string }[] = [
  { key: 'terminal', label: 'Terminal', index: '01' },
  { key: 'book', label: 'Order Book', index: '02' },
  { key: 'performance', label: 'Performance', index: '03' },
  { key: 'replay', label: 'Replay Lab', index: '04' },
  { key: 'reliability', label: 'Reliability', index: '05' },
  { key: 'architecture', label: 'Architecture', index: '06' },
];

interface ShellProps {
  page: PageKey;
  onNavigate: (page: PageKey) => void;
  state: EngineState | null;
  error: string | null;
  children: ReactNode;
}

export function Shell({ page, onNavigate, state, error, children }: ShellProps) {
  return (
    <div className="flex min-h-screen flex-col">
      <header className="sticky top-0 z-20 border-b border-line/80 bg-void/85 backdrop-blur-md">
        <div className="mx-auto flex w-full max-w-[1500px] flex-wrap items-center gap-x-5 gap-y-2 px-5 py-3">
          <div className="flex items-center gap-2.5">
            <svg width="26" height="26" viewBox="0 0 32 32" aria-hidden className="shrink-0">
              <rect width="32" height="32" rx="7" fill="#0e131c" stroke="#1c2634" />
              <path d="M8 21h4v-7H8zm6 0h4V9h-4zm6 0h4v-11h-4z" fill="#35e0c8" />
            </svg>
            <div className="leading-none">
              <div className="text-[15px] font-semibold tracking-tight text-chalk">UltraLOB</div>
              <div className="mt-1 text-[9px] uppercase tracking-[0.22em] text-fog/80">
                engineering console
              </div>
            </div>
          </div>

          <nav className="order-3 -mx-1 flex w-full flex-wrap gap-1 lg:order-none lg:w-auto">
            {NAV.map((item) => (
              <button
                key={item.key}
                type="button"
                onClick={() => onNavigate(item.key)}
                className={cx(
                  'group rounded-md px-2.5 py-1.5 text-[11px] font-medium transition-colors',
                  page === item.key
                    ? 'bg-panel-2 text-mint'
                    : 'text-fog hover:bg-panel-2/60 hover:text-mist',
                )}
              >
                <span className="mr-1.5 text-[9px] tracking-widest text-fog/50 group-hover:text-fog/70">
                  {item.index}
                </span>
                {item.label}
              </button>
            ))}
          </nav>

          <div className="ml-auto flex items-center gap-3 text-[10px] uppercase tracking-[0.12em] text-fog">
            <span className="hidden items-center gap-1.5 sm:flex">
              <span className="h-1.5 w-1.5 rounded-full bg-mint shadow-[0_0_6px_#35e0c8]" />
              wasm engine online
            </span>
            {state && (
              <>
                <span className="num hidden sm:inline">seq {state.seq.toLocaleString()}</span>
                <span className="num hidden md:inline">{state.activeOrders} resting</span>
                <span className="num hidden lg:inline">crc {shortCrc(state.crc64)}</span>
              </>
            )}
            <span className="rounded border border-amber/40 bg-amber/10 px-1.5 py-0.5 text-[9px] font-semibold text-amber">
              simulated data
            </span>
          </div>
        </div>
        {error && (
          <div className="border-t border-ask/30 bg-ask/10 px-5 py-1.5 text-[11px] text-ask">
            engine error: {error}
          </div>
        )}
      </header>

      <main className="mx-auto w-full max-w-[1500px] flex-1 px-5 py-5">{children}</main>

      <footer className="border-t border-line/70 px-5 py-3">
        <div className="mx-auto flex w-full max-w-[1500px] flex-wrap items-center justify-between gap-2 text-[10px] text-fog/70">
          <span>
            C++20 engine · Emscripten/WASM in a Web Worker · deterministic synthetic feed
          </span>
          <span>
            Simulated market data. No real orders, no brokerage connectivity, no customer funds.
          </span>
        </div>
      </footer>
    </div>
  );
}
