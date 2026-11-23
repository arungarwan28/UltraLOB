import { useEffect, useState } from 'react';
import { Badge, Panel } from '../components/Panel';
import { formatInt, shortCrc } from '../lib/format';
import type { EngineClient } from '../hooks/useEngine';
import type { ChaosReport } from '../lib/types';

function counter(label: string, value: number, tone?: string) {
  return (
    <div className="flex items-baseline justify-between gap-2 border-b border-line-soft/50 py-1 text-[11px] last:border-0">
      <span className="text-fog">{label}</span>
      <span className={`num ${tone ?? 'text-mist'}`}>{formatInt(value)}</span>
    </div>
  );
}

export function ReliabilityPage({ engine }: { engine: EngineClient }) {
  const [reports, setReports] = useState<ChaosReport[] | null>(null);
  const [running, setRunning] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const run = async () => {
    setRunning(true);
    setError(null);
    try {
      const result = await engine.request<ChaosReport[]>({ type: 'chaosAll' });
      setReports(result);
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure));
    } finally {
      setRunning(false);
    }
  };

  useEffect(() => {
    void run();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const passed = reports?.filter((report) => report.passed).length ?? 0;

  return (
    <div className="flex flex-col gap-4">
      <Panel
        title="Fault injection lab"
        hint="scenario code lives in the C++ engine and is asserted by the test suite"
        actions={
          <div className="flex items-center gap-2">
            {reports && (
              <Badge tone={passed === reports.length ? 'mint' : 'ask'}>
                {passed}/{reports.length} passing
              </Badge>
            )}
            <button
              type="button"
              onClick={() => void run()}
              disabled={running}
              className="rounded border border-line px-2 py-1 text-[10px] uppercase tracking-wider text-fog transition-colors hover:border-mint/40 hover:text-mint disabled:opacity-50"
            >
              {running ? 'running…' : 'run all'}
            </button>
          </div>
        }
      >
        <div className="grid gap-3 lg:grid-cols-2">
          {(reports ?? []).map((report) => (
            <article
              key={report.scenario}
              className="flex flex-col gap-2 rounded-lg border border-line-soft bg-ink/50 p-3"
            >
              <header className="flex items-start justify-between gap-2">
                <div>
                  <h3 className="text-[12px] font-semibold text-chalk">{report.title}</h3>
                  <p className="mt-0.5 text-[10px] leading-relaxed text-fog/80">
                    {report.description}
                  </p>
                </div>
                <Badge tone={report.passed ? 'mint' : 'ask'}>
                  {report.passed ? 'pass' : 'fail'}
                </Badge>
              </header>

              <div className="grid gap-x-4 sm:grid-cols-2">
                <div>
                  {counter('detected', report.detected)}
                  {counter('gaps', report.gaps)}
                  {counter('duplicates', report.duplicates)}
                  {counter('buffered / drained', report.buffered + report.drained)}
                </div>
                <div>
                  {counter('engine rejects', report.rejected)}
                  {counter('retries', report.retries)}
                  {counter('counted drops', report.dropped)}
                  {counter(
                    'state matches',
                    report.state_matches ? 1 : 0,
                    report.state_matches ? 'text-mint' : 'text-ask',
                  )}
                </div>
              </div>

              <div className="grid grid-cols-2 gap-2 text-[10px]">
                <div className="rounded border border-line-soft bg-panel/60 px-2 py-1">
                  <div className="uppercase tracking-[0.1em] text-fog">baseline crc</div>
                  <div className="num mt-0.5 break-all text-mist">
                    {shortCrc(report.crc_baseline)}
                  </div>
                </div>
                <div className="rounded border border-line-soft bg-panel/60 px-2 py-1">
                  <div className="uppercase tracking-[0.1em] text-fog">observed crc</div>
                  <div
                    className={`num mt-0.5 break-all ${
                      report.state_matches ? 'text-mint' : 'text-ask'
                    }`}
                  >
                    {shortCrc(report.crc_observed)}
                  </div>
                </div>
              </div>

              <p className="rounded border border-line-soft bg-panel/40 px-2 py-1.5 text-[10px] leading-relaxed text-fog">
                {report.detail}
              </p>
            </article>
          ))}

          {reports === null &&
            Array.from({ length: 6 }).map((_, index) => (
              <div
                key={index}
                className="h-[210px] animate-pulse rounded-lg border border-line-soft bg-ink/40"
              />
            ))}
        </div>

        {error && <p className="mt-2 text-[11px] text-ask">chaos run failed: {error}</p>}
      </Panel>

      <Panel title="How to read these results">
        <div className="grid gap-4 text-[11px] leading-relaxed text-fog md:grid-cols-3">
          <div>
            <h4 className="mb-1 text-[10px] uppercase tracking-[0.12em] text-mist">
              detection, then behaviour
            </h4>
            <p>
              Every scenario reports what the engine <em>detected</em> and what it then{' '}
              <em>did</em>. Detection without a defined recovery is not reliability, so the
              policy (halt, resynchronise or buffer) is declared in the scenario.
            </p>
          </div>
          <div>
            <h4 className="mb-1 text-[10px] uppercase tracking-[0.12em] text-mist">
              checksum is the verdict
            </h4>
            <p>
              A scenario only passes when the resulting book matches the expected state, byte
              for byte. Counters can look healthy while state is corrupted; the CRC64 comparison
              cannot.
            </p>
          </div>
          <div>
            <h4 className="mb-1 text-[10px] uppercase tracking-[0.12em] text-mist">
              same code as the tests
            </h4>
            <p>
              These reports come from <span className="num">engine/src/chaos.cpp</span>, which
              <span className="num"> tests/chaos_test.cpp</span> asserts in CI. The browser is a
              view of the evidence, not a separate implementation of it.
            </p>
          </div>
        </div>
      </Panel>
    </div>
  );
}
