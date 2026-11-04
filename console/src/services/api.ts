import type { Envelope, HealthData, OpsStatusData } from '../types';

export interface FetchResult<T> {
  data: T;
  httpStatus: number;
}

async function get<T>(path: string, signal?: AbortSignal): Promise<FetchResult<T>> {
  const res = await fetch(path, { signal, cache: 'no-store' });
  if (res.status === 401) {
    throw new Error('Unauthorized');
  }
  const body = (await res.json()) as Envelope<T>;
  if (body.type === 'error') {
    throw new Error(String(body.data));
  }
  return { data: body.data, httpStatus: res.status };
}

export const api = {
  health: (signal?: AbortSignal) => get<HealthData>('/ops/health', signal),
  status: (signal?: AbortSignal) => get<OpsStatusData>('/ops/status', signal),
};
