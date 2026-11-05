import { describe, expect, it, vi } from 'vitest';
import { renderHook, act, waitFor } from '@testing-library/react';
import { usePolling, fetchJson, HttpError } from '../polling';

describe('fetchJson', () => {
  it('treats a 503 degraded health envelope as data, not a failure', async () => {
    const degraded = {
      status: 503,
      ok: false,
      json: () =>
        Promise.resolve({
          type: 'result',
          data: { status: 'degraded', checks: { plugins: { status: 'degraded' } } },
        }),
    };
    const fetchMock = vi.fn().mockResolvedValue(degraded);
    vi.stubGlobal('fetch', fetchMock);
    try {
      const data = await fetchJson<{ status: string }>('/ops/health');
      expect(data.status).toBe('degraded');
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it('still rejects a genuine 500 with an HttpError', async () => {
    const err = { status: 500, ok: false, json: () => Promise.resolve({}) };
    const fetchMock = vi.fn().mockResolvedValue(err);
    vi.stubGlobal('fetch', fetchMock);
    try {
      await expect(fetchJson('/ops/status')).rejects.toBeInstanceOf(HttpError);
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it.each([409, 429, 504])(
    'preserves HTTP %i status from an error envelope',
    async (status) => {
      const err = {
        status,
        ok: false,
        json: () => Promise.resolve({ type: 'error', message: 'rejected' }),
      };
      const fetchMock = vi.fn().mockResolvedValue(err);
      vi.stubGlobal('fetch', fetchMock);
      try {
        await expect(fetchJson('/ops/profile')).rejects.toMatchObject({
          name: 'HttpError',
          status,
        });
      } finally {
        vi.unstubAllGlobals();
      }
    },
  );
});

describe('usePolling race protection', () => {
  it('does not let a late response overwrite a newer query', async () => {
    // First query resolves slowly; a second query (refresh) resolves fast
    // with fresh data. The late first response must be dropped.
    let resolveFirst: (v: string) => void = () => {};
    const first = new Promise<string>((res) => { resolveFirst = res; });
    let calls = 0;
    const fetchFn = vi.fn((_signal: AbortSignal) => {
      calls++;
      if (calls === 1) return first;
      return Promise.resolve('fresh');
    });

    const { result } = renderHook(() =>
      usePolling(fetchFn, { intervalMs: 99999, pauseWhenHidden: false }),
    );

    // Trigger a second query via refresh while the first is still in flight.
    act(() => { result.current.refresh(); });
    // The second query resolves 'fresh' first.
    await waitFor(() => expect(result.current.data).toBe('fresh'));

    // Now the first (stale) response arrives late — it must NOT overwrite.
    await act(async () => { resolveFirst('stale'); });
    expect(result.current.data).toBe('fresh');
    expect(result.current.stale).toBe(false);
  });

  it('keeps last good data and marks stale on network failure', async () => {
    let calls = 0;
    const fetchFn = vi.fn((_signal: AbortSignal) => {
      calls++;
      if (calls === 1) return Promise.resolve('ok');
      return Promise.reject(new Error('network down'));
    });

    const { result } = renderHook(() =>
      usePolling(fetchFn, { intervalMs: 99999, pauseWhenHidden: false }),
    );

    await waitFor(() => expect(result.current.data).toBe('ok'));
    // Force a second poll that fails.
    act(() => { result.current.refresh(); });
    await waitFor(() => expect(result.current.stale).toBe(true));
    expect(result.current.data).toBe('ok');
    expect(result.current.status).toBe('ready');
  });

  it('uses the normal interval after a success followed by a failure', async () => {
    // A successful snapshot must flip the backoff decision to the normal
    // interval; a subsequent failure must NOT fall back to exponential
    // backoff as if there were never any data.
    let calls = 0;
    const fetchFn = vi.fn((_signal: AbortSignal) => {
      calls++;
      if (calls === 1) return Promise.resolve('ok');
      if (calls === 2) return Promise.reject(new Error('network down'));
      return Promise.resolve('ok');
    });

    const { result } = renderHook(() =>
      usePolling(fetchFn, { intervalMs: 50, initialBackoffMs: 10000, pauseWhenHidden: false }),
    );

    await waitFor(() => expect(result.current.data).toBe('ok'));
    // Second poll fails; with everHadData true the next poll uses the normal
    // interval (50ms), not the 10s backoff, so a third call happens fast.
    await waitFor(() => expect(result.current.stale).toBe(true));
    await waitFor(() => expect(calls).toBeGreaterThanOrEqual(3));
  }, 5000);
});
