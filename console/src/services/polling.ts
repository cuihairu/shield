import { useEffect, useRef, useState } from 'react';
import type { Envelope } from '../types';

export type PollStatus = 'loading' | 'ready' | 'empty' | 'error' | 'unauthorized';

// An HTTP error carrying the status code so the UI can branch on specific
// codes (409/429/504) instead of a generic message.
export class HttpError extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(message);
    this.name = 'HttpError';
    this.status = status;
  }
}

export interface PollState<T> {
  status: PollStatus;
  data: T | null;
  stale: boolean;
  error: string | null;
  lastUpdated: number | null;
}

export interface PollOptions<T> {
  intervalMs?: number;       // 2s baseline, configurable
  pauseWhenHidden?: boolean; // optional pause when the tab is hidden
  backoffFactor?: number;    // exponential backoff on failure (default 2)
  maxBackoffMs?: number;
  initialBackoffMs?: number;
  hasData?: (data: T) => boolean; // decide ready vs empty
}

export interface PollResult<T> extends PollState<T> {
  refresh: () => void;
}

// Bearer token entry point for the ops auth gate. The token is kept in
// sessionStorage (NOT localStorage) and attached to every /ops request:
// it is deliberately session-scoped so closing the tab discards it and it
// never survives a browser restart. An empty token means the gate is off or
// the user has not authenticated.
const TOKEN_KEY = 'shield_ops_token';

export function getOpsToken(): string {
  try {
    return sessionStorage.getItem(TOKEN_KEY) ?? '';
  } catch {
    return '';
  }
}

export function setOpsToken(token: string): void {
  try {
    if (token) sessionStorage.setItem(TOKEN_KEY, token);
    else sessionStorage.removeItem(TOKEN_KEY);
  } catch {
    // localStorage unavailable (private mode / SSR): the token simply is not
    // persisted, which is the safe fallback.
  }
}

function authHeaders(): Record<string, string> {
  const token = getOpsToken();
  return token ? { Authorization: `Bearer ${token}` } : {};
}

// Type-safe fetch of a {type,data} envelope with timeout + cancellation.
export async function fetchJson<T>(
  path: string,
  signal?: AbortSignal,
  timeoutMs = 8000,
  tokenOverride?: string,
  init?: { method?: string; body?: unknown },
): Promise<T> {
  // Combine the caller's cancel signal with a timeout.
  const controller = new AbortController();
  const onAbort = () => controller.abort();
  signal?.addEventListener('abort', onAbort);
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(path, {
      signal: controller.signal,
      cache: 'no-store',
      method: init?.method ?? 'GET',
      body: init?.body !== undefined ? JSON.stringify(init.body) : undefined,
      headers: tokenOverride ? { Authorization: `Bearer ${tokenOverride}` } : authHeaders(),
    });
    if (res.status === 401) {
      throw new Error('Unauthorized');
    }
    if (!res.ok) {
      let detail = `HTTP ${res.status}`;
      try {
        const errBody = (await res.json()) as { type?: string; data?: unknown; message?: string };
        if (errBody && errBody.message) detail = String(errBody.message);
      } catch {
        // non-JSON error body: keep the status-based message
      }
      throw new HttpError(res.status, detail);
    }
    const body = (await res.json()) as Envelope<T>;
    if (body.type === 'error') {
      const data = body.data ?? (body as { message?: string }).message;
      throw new Error(String(data ?? 'error'));
    }
    return body.data;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', onAbort);
  }
}

// Polling hook with race protection: a late response never overwrites a
// newer query. On failure it backs off exponentially and marks data stale
// (keeps last good snapshot, never pretends it is current).
export function usePolling<T>(
  fetchFn: (signal: AbortSignal) => Promise<T>,
  options: PollOptions<T> = {},
): PollResult<T> {
  const {
    intervalMs = 2000,
    pauseWhenHidden = true,
    backoffFactor = 2,
    maxBackoffMs = 30000,
    initialBackoffMs = 2000,
    hasData,
  } = options;

  const [state, setState] = useState<PollState<T>>({
    status: 'loading',
    data: null,
    stale: false,
    error: null,
    lastUpdated: null,
  });

  // Monotonic query id: only the latest in-flight query may commit.
  const queryIdRef = useRef(0);
  const abortRef = useRef<AbortController | null>(null);
  const [tick, setTick] = useState(0);

  useEffect(() => {
    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | null = null;
    let backoff = initialBackoffMs;

    async function run() {
      const id = ++queryIdRef.current;
      const controller = new AbortController();
      abortRef.current?.abort();
      abortRef.current = controller;
    // Track the latest data inside the effect so the backoff decision below
    // reads the live snapshot, not the initial state captured at mount.
    let hasDataRef = false;
      try {
        const data = await fetchFn(controller.signal);
        if (cancelled || id !== queryIdRef.current) return; // race: drop stale
      hasDataRef = data !== null;
        setState({
          status: hasData && !hasData(data) ? 'empty' : 'ready',
          data,
          stale: false,
          error: null,
          lastUpdated: Date.now(),
        });
        backoff = initialBackoffMs;
      } catch (e) {
        if (cancelled || controller.signal.aborted && id !== queryIdRef.current) return;
        if (id !== queryIdRef.current) return;
        const msg = e instanceof Error ? e.message : String(e);
        if (msg === 'Unauthorized') {
          setState((s) => ({ ...s, status: 'unauthorized' }));
          return;
        }
        // Keep last snapshot but mark stale; only the very first failure
        // (no data yet) surfaces as an error page.
        setState((s) => ({
          ...s,
          stale: s.data !== null,
          status: s.data === null ? 'error' : 'ready',
          error: s.data === null ? msg : null,
        }));
      }
      if (cancelled) return;
      // Schedule next poll with exponential backoff after failures.
      const delay = hasDataRef ? intervalMs : backoff;
      timer = setTimeout(run, delay);
      if (!hasDataRef) {
        backoff = Math.min(backoff * backoffFactor, maxBackoffMs);
      }
    }

    // Pause when the tab is hidden (optional).
    const onVisibility = () => {
      if (!pauseWhenHidden) return;
      if (document.hidden) {
        if (timer) { clearTimeout(timer); timer = null; }
      } else {
        if (timer === null) run();
      }
    };

    run();
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      cancelled = true;
      abortRef.current?.abort();
      if (timer) clearTimeout(timer);
      document.removeEventListener('visibilitychange', onVisibility);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tick]);

  return {
    ...state,
    refresh: () => setTick((t) => t + 1),
  };
}

// Raw text fetch for Prometheus exposition (not the JSON envelope). Same
// timeout/cancel semantics as fetchJson.
export async function fetchText(
  path: string,
  signal?: AbortSignal,
  timeoutMs = 8000,
  tokenOverride?: string,
): Promise<string> {
  const controller = new AbortController();
  const onAbort = () => controller.abort();
  signal?.addEventListener('abort', onAbort);
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(path, {
      signal: controller.signal,
      cache: 'no-store',
      headers: tokenOverride ? { Authorization: `Bearer ${tokenOverride}` } : authHeaders(),
    });
    if (res.status === 401) throw new Error('Unauthorized');
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res.text();
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', onAbort);
  }
}
