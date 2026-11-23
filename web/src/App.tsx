import { useEffect, useState } from 'react';
import { Shell, type PageKey } from './components/Shell';
import { useEngine } from './hooks/useEngine';
import { ArchitecturePage } from './pages/Architecture';
import { BookPage } from './pages/Book';
import { PerformancePage } from './pages/Performance';
import { ReliabilityPage } from './pages/Reliability';
import { ReplayPage } from './pages/Replay';
import { TerminalPage } from './pages/Terminal';

const PAGES: PageKey[] = [
  'terminal',
  'book',
  'performance',
  'replay',
  'reliability',
  'architecture',
];

function readHash(): PageKey {
  const candidate = window.location.hash.replace(/^#\/?/, '') as PageKey;
  return PAGES.includes(candidate) ? candidate : 'terminal';
}

export default function App() {
  const [page, setPage] = useState<PageKey>(readHash);
  const engine = useEngine();

  useEffect(() => {
    const onHashChange = () => setPage(readHash());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  useEffect(() => {
    window.scrollTo({ top: 0, behavior: 'smooth' });
  }, [page]);

  const navigate = (next: PageKey) => {
    window.location.hash = `#/${next}`;
    setPage(next);
  };

  return (
    <Shell page={page} onNavigate={navigate} state={engine.state} error={engine.error}>
      {page === 'terminal' && <TerminalPage engine={engine} />}
      {page === 'book' && <BookPage engine={engine} />}
      {page === 'performance' && <PerformancePage engine={engine} />}
      {page === 'replay' && <ReplayPage engine={engine} />}
      {page === 'reliability' && <ReliabilityPage engine={engine} />}
      {page === 'architecture' && <ArchitecturePage />}
    </Shell>
  );
}
