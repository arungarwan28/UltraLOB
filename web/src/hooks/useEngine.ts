import { useCallback, useEffect, useRef, useState } from 'react';
import type { EngineState } from '../lib/types';
import type { WorkerRequest, WorkerResponse } from '../workers/protocol';

// Distributive omit: WorkerRequest is a union, and a plain Omit over a union
// collapses it to the shared keys only.
type DistributiveOmit<T, K extends PropertyKey> = T extends unknown ? Omit<T, K> : never;
type RequestBody = DistributiveOmit<WorkerRequest, 'id'>;

interface Pending {
  resolve: (payload: unknown) => void;
  reject: (error: Error) => void;
}

export interface EngineClient {
  state: EngineState | null;
  error: string | null;
  busy: boolean;
  request: <T>(body: RequestBody) => Promise<T>;
  submitLimit: (side: 0 | 1, price: number, qty: number) => Promise<EngineState>;
  submitMarket: (side: 0 | 1, qty: number) => Promise<EngineState>;
  cancel: (orderId: number) => Promise<EngineState>;
  modify: (orderId: number, price: number, qty: number) => Promise<EngineState>;
  reset: () => Promise<void>;
  refresh: () => Promise<void>;
}

interface WorkerWaiter {
  resolve: (worker: Worker) => void;
  reject: (error: Error) => void;
}

export function useEngine(): EngineClient {
  const workerRef = useRef<Worker | null>(null);
  // Child effects run before the App's effect that creates the worker, so a
  // page that loads data on mount (the reliability lab) used to fail with
  // "engine worker not ready". Requests wait here until a worker exists.
  const waiters = useRef<WorkerWaiter[]>([]);
  const pending = useRef(new Map<number, Pending>());
  const nextId = useRef(1);
  const [state, setState] = useState<EngineState | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    const worker = new Worker(new URL('../workers/engine.worker.ts', import.meta.url), {
      type: 'module',
    });
    workerRef.current = worker;
    const queuedWaiters = waiters.current;
    waiters.current = [];
    for (const waiter of queuedWaiters) waiter.resolve(worker);

    worker.addEventListener('message', (event: MessageEvent<WorkerResponse>) => {
      const message = event.data;
      const waiter = pending.current.get(message.id);
      pending.current.delete(message.id);
      if (message.type === 'error') {
        setError(message.message);
        waiter?.reject(new Error(message.message));
        return;
      }
      if (message.type === 'state') {
        setState(message.payload);
        waiter?.resolve(message.payload);
        return;
      }
      waiter?.resolve(message.payload);
    });

    worker.addEventListener('error', (event) => {
      setError(event.message || 'engine worker failed to start');
    });

    void requestRef.current?.({ type: 'snapshot' }).catch(() => undefined);

    return () => {
      // Settle anything still in flight: a terminated worker never replies, so
      // leaving these promises pending would leak and report `busy` forever
      // (React StrictMode mounts the effect twice in development).
      for (const [, waiter] of pending.current) {
        waiter.reject(new Error('engine worker stopped'));
      }
      pending.current.clear();
      const queued = waiters.current;
      waiters.current = [];
      for (const waiter of queued) waiter.reject(new Error('engine worker stopped'));
      worker.terminate();
      workerRef.current = null;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const request = useCallback(async <T,>(body: RequestBody): Promise<T> => {
    let worker = workerRef.current;
    if (worker === null) {
      worker = await new Promise<Worker>((resolve, reject) => {
        waiters.current.push({ resolve, reject });
      });
    }
    const id = nextId.current;
    nextId.current += 1;
    setBusy(true);
    return new Promise<T>((resolve, reject) => {
      pending.current.set(id, {
        resolve: (payload) => {
          setBusy(false);
          resolve(payload as T);
        },
        reject: (failure) => {
          setBusy(false);
          reject(failure);
        },
      });
      worker.postMessage({ ...body, id } as WorkerRequest);
    });
  }, []);

  const requestRef = useRef(request);
  requestRef.current = request;

  const submitLimit = useCallback(
    (side: 0 | 1, price: number, qty: number) =>
      request<EngineState>({ type: 'submitLimit', side, price, qty }),
    [request],
  );

  const submitMarket = useCallback(
    (side: 0 | 1, qty: number) => request<EngineState>({ type: 'submitMarket', side, qty }),
    [request],
  );

  const cancel = useCallback(
    (orderId: number) => request<EngineState>({ type: 'cancel', orderId }),
    [request],
  );

  const modify = useCallback(
    (orderId: number, price: number, qty: number) =>
      request<EngineState>({ type: 'modify', orderId, price, qty }),
    [request],
  );

  const reset = useCallback(async () => {
    await request<EngineState>({ type: 'reset' });
  }, [request]);

  const refresh = useCallback(async () => {
    await request<EngineState>({ type: 'snapshot' });
  }, [request]);

  return { state, error, busy, request, submitLimit, submitMarket, cancel, modify, reset, refresh };
}
