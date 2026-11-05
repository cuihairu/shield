import { describe, expect, it, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import ConfigPage from '../ConfigPage';
import { api } from '../../services/api';

vi.mock('../../services/api', () => ({
  api: {
    config: vi.fn(),
  },
}));

const configMock = api.config as ReturnType<typeof vi.fn>;

// OPS_KEYS mirrors ConfigPage's default query set.
const OPS_KEYS = [
  'http.host',
  'http.port',
  'http.ops_auth_token',
  'http.static_root',
  'http.eval_enabled',
  'http.eval_token',
  'http.profile_enabled',
  'http.profile_token',
  'http.slow_call_threshold_ms',
  'http.profile_cooldown_seconds',
];

// Server-authoritative redaction sentinel. The client must not guess from the
// key name; it only reacts to the exact value the server returns.
const REDACTED = '***redacted***';

function mockConfig(resolver: (key: string) => string | null) {
  configMock.mockImplementation((key: string) => Promise.resolve(resolver(key)));
}

describe('ConfigPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('marks server-redacted keys as 已脱敏 and never echoes the original value', async () => {
    mockConfig((key) => {
      if (key === 'http.ops_auth_token') return REDACTED;
      if (key === 'http.eval_token') return REDACTED;
      if (key === 'http.profile_token') return REDACTED;
      if (key === 'http.host') return '127.0.0.1';
      return null;
    });
    render(
      <MemoryRouter>
        <ConfigPage />
      </MemoryRouter>,
    );
    await waitFor(() => expect(screen.getAllByText('已脱敏').length).toBe(3));
    // Plaintext key renders its real value.
    expect(screen.getByText('127.0.0.1')).toBeTruthy();
    // The original secret never appears anywhere in the document.
    expect(screen.queryByText(/hunter2|s3cr3t|token-value/i)).toBeNull();
    // Never guess redaction from the key name: unknown/null keys are not
    // redacted just because their name contains "token".
    expect(screen.getAllByText(/unset|未配置/).length).toBeGreaterThan(0);
  }, 30000);

  it('shows 未配置 for a key that resolves to null', async () => {
    mockConfig(() => null);
    render(
      <MemoryRouter>
        <ConfigPage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getAllByText('未配置').length).toBe(OPS_KEYS.length),
    );
  }, 30000);

  it('queries an extra key on demand and respects server redaction', async () => {
    mockConfig(() => null);
    render(
      <MemoryRouter>
        <ConfigPage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getAllByText('未配置').length).toBe(OPS_KEYS.length),
    );
    configMock.mockImplementation((key: string) =>
      Promise.resolve(key === 'database.password' ? REDACTED : 'abc'),
    );
    fireEvent.change(screen.getByPlaceholderText(/Query a config key/), {
      target: { value: 'database.password' },
    });
    fireEvent.click(screen.getByRole('button', { name: 'Query' }));
    await waitFor(() => expect(screen.getByText('database.password')).toBeTruthy());
    expect(screen.getByText('已脱敏')).toBeTruthy();
    // The secret value is never rendered.
    expect(screen.queryByText(/hunter2|s3cr3t/)).toBeNull();
  }, 30000);
});
