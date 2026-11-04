import { fetchJson, fetchText } from './polling';
import type { HealthData, OpsStatusData, PluginInstance, ServiceDetail } from '../types';

// Typed client over the FIX-WEB-01 contract. Every call returns the
// envelope's `data`, throws a typed Error on non-2xx / error envelope,
// and honours the caller's AbortSignal for cancellation.
export const api = {
  health: (signal?: AbortSignal) => fetchJson<HealthData>('/ops/health', signal),
  status: (signal?: AbortSignal) => fetchJson<OpsStatusData>('/ops/status', signal),
  services: (signal?: AbortSignal) => fetchJson<string[] | 'timeout'>('/ops/services', signal),
  serviceDetail: (name: string, signal?: AbortSignal) =>
    fetchJson<ServiceDetail>(`/ops/services/${encodeURIComponent(name)}`, signal),
  plugins: (signal?: AbortSignal) => fetchJson<PluginInstance[]>('/ops/plugins', signal),
  metricsText: (signal?: AbortSignal) => fetchText('/ops/metrics', signal),
};
