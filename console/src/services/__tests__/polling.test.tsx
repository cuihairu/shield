import { describe, expect, it, vi } from 'vitest';
import { renderHook, act, waitFor } from '@testing-library/react';
import { usePolling } from '../polling';

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
});
