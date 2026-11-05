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
  config: (key: string, signal?: AbortSignal) =>
    fetchJson<string | null>(`/ops/config?key=${encodeURIComponent(key)}`, signal),
  metricsText: (signal?: AbortSignal) => fetchText('/ops/metrics', signal),
  // Control endpoints (opt-in). The caller supplies the endpoint-specific
  // token as the Bearer header via the token-aware fetch helpers.
  evalCode: (code: string, token: string, signal?: AbortSignal) =>
    fetchJson<unknown>('/ops/eval', signal, 8000, token, { method: 'POST', body: { code } }),
  profile: (action: string, body: Record<string, unknown>, token: string, signal?: AbortSignal) =>
    fetchJson<unknown>('/ops/profile', signal, 8000, token, { method: 'POST', body: { action, ...body } }),
};
