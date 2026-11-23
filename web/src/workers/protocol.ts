import type {
  BenchResult,
  ChaosReport,
  EngineState,
  GapPolicy,
  ReplayState,
  Side,
  VerifyResult,
  Workload,
} from '../lib/types';

export type WorkerRequest =
  | { id: number; type: 'snapshot' }
  | { id: number; type: 'submitLimit'; side: Side; price: number; qty: number }
  | { id: number; type: 'submitMarket'; side: Side; qty: number }
  | { id: number; type: 'cancel'; orderId: number }
  | { id: number; type: 'modify'; orderId: number; price: number; qty: number }
  | { id: number; type: 'hasOrder'; orderId: number }
  | { id: number; type: 'reset' }
  | { id: number; type: 'benchmark'; events: number; seed: number; workload: Workload }
  | {
      id: number;
      type: 'replayBegin';
      events: number;
      seed: number;
      workload: Workload;
      gapPolicy: GapPolicy;
    }
  | { id: number; type: 'replayStep'; steps: number }
  | { id: number; type: 'replayState' }
  | { id: number; type: 'replayVerify' }
  | { id: number; type: 'chaosAll' }
  | { id: number; type: 'chaosOne'; scenario: number };

export interface WorkerStateResponse {
  id: number;
  type: 'state';
  payload: EngineState;
}

export interface WorkerValueResponse {
  id: number;
  type: 'value';
  payload: unknown;
}

export type WorkerSuccess =
  | WorkerStateResponse
  | { id: number; type: 'bench'; payload: BenchResult }
  | { id: number; type: 'replay'; payload: ReplayState }
  | { id: number; type: 'verify'; payload: VerifyResult }
  | { id: number; type: 'chaos'; payload: ChaosReport[] }
  | WorkerValueResponse;

export interface WorkerFailure {
  id: number;
  type: 'error';
  message: string;
}

export type WorkerResponse = WorkerSuccess | WorkerFailure;
