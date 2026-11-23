// The engine runs here, off the UI thread. Every order, benchmark and replay
// below is executed by the C++ engine compiled to WebAssembly — this file only
// marshals messages and copies flat buffers into JS objects.
import createUltralobModule from '../wasm/ultralob.mjs';
import { UltralobEngine, type UltralobWasm } from '../wasm/engine';
import type { WorkerRequest, WorkerResponse } from './protocol';

let engine: UltralobEngine | null = null;
let ready: Promise<UltralobEngine> | null = null;

// A deterministic starting book so the console opens on a live market instead of
// an empty one. Still simulated data, still produced by the same C++ engine.
function seedDemoBook(live: UltralobEngine): void {
  const mid = 10000; // 100.00 in ticks
  for (let level = 0; level < 5; level += 1) {
    for (let slice = 0; slice < 2; slice += 1) {
      const size = 30 + level * 20 + slice * 25;
      live.submitLimit(0, mid - 1 - level * 5, size);
      live.submitLimit(1, mid + 1 + level * 5, size);
    }
  }
}

function load(): Promise<UltralobEngine> {
  if (ready !== null) return ready;
  ready = createUltralobModule({
    locateFile: (path: string) => `${import.meta.env.BASE_URL}wasm/${path}`,
  }).then((module) => {
    engine = new UltralobEngine(module as unknown as UltralobWasm);
    seedDemoBook(engine);
    return engine;
  });
  // Do not poison the worker after a transient load failure: clear the cached
  // promise so the next request retries instead of replaying the first error
  // for the lifetime of the tab.
  ready = ready.catch((error: unknown) => {
    ready = null;
    engine = null;
    throw new Error(
      `could not load the WASM engine: ${error instanceof Error ? error.message : String(error)}`,
    );
  });
  return ready;
}

function reply(message: WorkerResponse): void {
  (self as unknown as Worker).postMessage(message);
}

async function handle(request: WorkerRequest): Promise<void> {
  try {
    const live = await load();
    switch (request.type) {
      case 'snapshot':
        reply({ id: request.id, type: 'state', payload: live.snapshot() });
        return;
      case 'submitLimit':
        live.submitLimit(request.side, request.price, request.qty);
        reply({ id: request.id, type: 'state', payload: live.snapshot() });
        return;
      case 'submitMarket':
        live.submitMarket(request.side, request.qty);
        reply({ id: request.id, type: 'state', payload: live.snapshot() });
        return;
      case 'cancel':
        live.cancel(request.orderId);
        reply({ id: request.id, type: 'state', payload: live.snapshot() });
        return;
      case 'modify':
        live.modify(request.orderId, request.price, request.qty);
        reply({ id: request.id, type: 'state', payload: live.snapshot() });
        return;
      case 'hasOrder':
        reply({
          id: request.id,
          type: 'value',
          payload: { exists: live.hasOrder(request.orderId), remaining: live.orderRemaining(request.orderId) },
        });
        return;
      case 'reset':
        live.reset();
        seedDemoBook(live);
        reply({ id: request.id, type: 'state', payload: live.snapshot() });
        return;
      case 'benchmark':
        reply({
          id: request.id,
          type: 'bench',
          payload: live.benchmark(request.events, request.seed, request.workload),
        });
        return;
      case 'replayBegin':
        live.replayBegin(request.events, request.seed, request.workload, request.gapPolicy);
        reply({ id: request.id, type: 'replay', payload: live.replayState() });
        return;
      case 'replayStep':
        live.replayStep(request.steps);
        reply({ id: request.id, type: 'replay', payload: live.replayState() });
        return;
      case 'replayState':
        reply({ id: request.id, type: 'replay', payload: live.replayState() });
        return;
      case 'replayVerify':
        reply({ id: request.id, type: 'verify', payload: live.replayVerify() });
        return;
      case 'chaosAll':
        reply({ id: request.id, type: 'chaos', payload: live.chaosAll() });
        return;
      case 'chaosOne':
        reply({ id: request.id, type: 'chaos', payload: [live.chaos(request.scenario)] });
        return;
    }
  } catch (error) {
    reply({
      id: request.id,
      type: 'error',
      message: error instanceof Error ? error.message : String(error),
    });
  }
}

self.addEventListener('message', (event: MessageEvent<WorkerRequest>) => {
  void handle(event.data);
});
