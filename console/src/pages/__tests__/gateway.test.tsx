import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import GatewayPage from '../GatewayPage';
import { api } from '../../services/api';

vi.mock('../../services/api', () => ({
  api: {
    metricsText: vi.fn(),
  },
}));

const sample = (extra = '') =>
  '# TYPE shield_gateway_connections_total counter\n' +
  'shield_gateway_connections_total{port="9000"} 42\n' +
  '# TYPE shield_gateway_active_sessions gauge\n' +
  'shield_gateway_active_sessions{port="9000"} 7\n' +
  '# TYPE shield_gateway_rate_limited_messages_total counter\n' +
  'shield_gateway_rate_limited_messages_total{port="9000"} 3\n' +
  '# TYPE shield_gateway_rejections_total counter\n' +
  'shield_gateway_rejections_total{port="9000",reason="blocked_ip"} 1\n' +
  '# TYPE shield_gateway_tls_handshakes_total counter\n' +
  'shield_gateway_tls_handshakes_total{port="9000"} 12\n' +
  '# TYPE shield_gateway_tls_handshake_failures_total counter\n' +
  'shield_gateway_tls_handshake_failures_total{port="9000"} 2\n' +
  extra;

describe('GatewayPage', () => {
  beforeEach(() => vi.clearAllMocks());
  afterEach(() => vi.restoreAllMocks());

  it('renders per-port counters and gauge from /ops/metrics', async () => {
    (api.metricsText as ReturnType<typeof vi.fn>).mockResolvedValue(sample());
    render(
      <MemoryRouter>
        <GatewayPage />
      </MemoryRouter>,
    );
    await waitFor(() => expect(screen.getByText('9000')).toBeTruthy());
    expect(screen.getByText('42')).toBeTruthy();
    expect(screen.getByText('7')).toBeTruthy();
    expect(screen.getByText('3')).toBeTruthy();
    expect(screen.getByText('12')).toBeTruthy();
    expect(screen.getByText('2')).toBeTruthy();
    expect(screen.getByText('1')).toBeTruthy();
    expect(screen.getByText(/Active sessions \(gauge\)/)).toBeTruthy();
    expect(screen.getByText(/Rate limited \(counter\)/)).toBeTruthy();
    expect(screen.getByText(/TLS ok \(counter\)/)).toBeTruthy();
    expect(screen.getByText(/TLS fail \(counter\)/)).toBeTruthy();
  }, 30000);

  it('shows unauthorized when the ops auth gate denies access', async () => {
    (api.metricsText as ReturnType<typeof vi.fn>).mockRejectedValue(
      new Error('Unauthorized'),
    );
    render(
      <MemoryRouter>
        <GatewayPage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getByText(/Authentication required/)).toBeTruthy(),
    );
  }, 30000);

  it('marks data stale after a failure following a good snapshot', async () => {
    (api.metricsText as ReturnType<typeof vi.fn>).mockResolvedValue(sample());
    render(
      <MemoryRouter>
        <GatewayPage />
      </MemoryRouter>,
    );
    await waitFor(() => expect(screen.getByText('9000')).toBeTruthy());
    // Next poll fails: keep last snapshot, mark stale.
    (api.metricsText as ReturnType<typeof vi.fn>).mockRejectedValue(
      new Error('network down'),
    );
    await waitFor(
      () => expect(screen.getByText(/Data is stale/)).toBeTruthy(),
      { timeout: 6000 },
    );
    // The last good snapshot is still on screen.
    expect(screen.getByText('42')).toBeTruthy();
  }, 30000);
});
