import type { ReactNode } from 'react';
import { cx } from '../lib/cx';

interface PanelProps {
  title?: string;
  hint?: string;
  actions?: ReactNode;
  children: ReactNode;
  className?: string;
  bodyClassName?: string;
}

export function Panel({ title, hint, actions, children, className, bodyClassName }: PanelProps) {
  return (
    <section
      className={cx(
        'flex min-h-0 flex-col rounded-xl border border-line/90 bg-panel/70 shadow-[0_1px_0_rgba(255,255,255,0.03)_inset]',
        className,
      )}
    >
      {(title || actions) && (
        <header className="flex flex-wrap items-center justify-between gap-x-3 gap-y-1 border-b border-line-soft px-4 py-2.5">
          <div className="flex min-w-0 flex-1 items-baseline gap-2">
            <h2 className="truncate text-[11px] font-semibold uppercase tracking-[0.16em] text-fog">
              {title}
            </h2>
            {hint && <span className="truncate text-[11px] text-fog/70">{hint}</span>}
          </div>
          {actions}
        </header>
      )}
      <div className={cx('min-h-0 flex-1 p-4', bodyClassName)}>{children}</div>
    </section>
  );
}

export function Badge({
  children,
  tone = 'neutral',
  className,
}: {
  children: ReactNode;
  tone?: 'neutral' | 'mint' | 'amber' | 'ask' | 'bid' | 'violet';
  className?: string;
}) {
  const tones: Record<string, string> = {
    neutral: 'border-line bg-panel-2 text-fog',
    mint: 'border-mint/40 bg-mint/10 text-mint',
    amber: 'border-amber/40 bg-amber/10 text-amber',
    ask: 'border-ask/40 bg-ask/10 text-ask',
    bid: 'border-bid/40 bg-bid/10 text-bid',
    violet: 'border-violet/40 bg-violet/10 text-violet',
  };
  return (
    <span
      className={cx(
        'inline-flex items-center gap-1 rounded-md border px-1.5 py-0.5 text-[10px] font-medium uppercase tracking-[0.1em]',
        tones[tone],
        className,
      )}
    >
      {children}
    </span>
  );
}

export function StatTile({
  label,
  value,
  sub,
  tone = 'default',
}: {
  label: string;
  value: ReactNode;
  sub?: ReactNode;
  tone?: 'default' | 'mint' | 'ask' | 'bid' | 'amber';
}) {
  const tones: Record<string, string> = {
    default: 'text-chalk',
    mint: 'text-mint',
    ask: 'text-ask',
    bid: 'text-bid',
    amber: 'text-amber',
  };
  return (
    <div className="rounded-lg border border-line-soft bg-ink/60 px-3 py-2.5">
      <div className="text-[10px] font-medium uppercase tracking-[0.14em] text-fog">{label}</div>
      <div className={cx('num mt-1 text-[15px] leading-tight', tones[tone])}>{value}</div>
      {sub && <div className="mt-0.5 text-[10px] text-fog/80">{sub}</div>}
    </div>
  );
}
