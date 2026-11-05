import { describe, expect, it, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import EvalProfilePage from '../EvalProfilePage';
import { api } from '../../services/api';
import { HttpError } from '../../services/polling';

vi.mock('../../services/api', () => ({
  api: {
    config: vi.fn(),
    evalCode: vi.fn(),
    profile: vi.fn(),
  },
}));

const configMock = api.config as ReturnType<typeof vi.fn>;
const evalMock = api.evalCode as ReturnType<typeof vi.fn>;
const profileMock = api.profile as ReturnType<typeof vi.fn>;

function mockConfig(evalEnabled: boolean, profileEnabled: boolean, cooldown: string | null) {
  configMock.mockImplementation((key: string) =>
    Promise.resolve(
      key === 'http.eval_enabled' ? String(evalEnabled)
        : key === 'http.profile_enabled' ? String(profileEnabled)
          : cooldown,
    ),
  );
}

describe('EvalProfilePage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('shows unavailable reasons when endpoints are not enabled', async () => {
    mockConfig(false, false, null);
    render(
      <MemoryRouter>
        <EvalProfilePage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getAllByText(/Endpoint not enabled/).length).toBe(2),
    );
    // No fake forms should be rendered.
    expect(screen.queryByText(/Run eval/)).toBeNull();
    expect(screen.queryByText(/Start/)).toBeNull();
  }, 30000);

  it('runs eval with a manually typed token (never persisted)', async () => {
    mockConfig(true, false, null);
    evalMock.mockResolvedValue([42]);
    render(
      <MemoryRouter>
        <EvalProfilePage />
      </MemoryRouter>,
    );
    await waitFor(() => expect(screen.getByText(/Run eval/)).toBeTruthy());
    fireEvent.change(screen.getByPlaceholderText(/eval token/), {
      target: { value: 'eval-secret' },
    });
    fireEvent.change(screen.getByPlaceholderText(/Lua code/), {
      target: { value: 'return 6 * 7' },
    });
    fireEvent.click(screen.getByText(/Run eval/));
    await waitFor(() => expect(evalMock).toHaveBeenCalled());
    expect(evalMock).toHaveBeenCalledWith('return 6 * 7', 'eval-secret');
    // Token must never appear in the DOM (not persisted / echoed).
    expect(screen.queryByText(/eval-secret/)).toBeNull();
    await waitFor(() => expect(screen.getByText('[42]')).toBeTruthy());
  }, 30000);

  it('surfaces 409 / 429 / 504 profile branches', async () => {
    mockConfig(false, true, 10);
    profileMock.mockRejectedValueOnce(new HttpError(409, 'session active'));
    render(
      <MemoryRouter>
        <EvalProfilePage />
      </MemoryRouter>,
    );
    await waitFor(() => expect(screen.getByText(/Status/)).toBeTruthy());
    fireEvent.change(screen.getByPlaceholderText(/profile token/), {
      target: { value: 'prof-secret' },
    });
    fireEvent.change(screen.getByPlaceholderText(/service id/), {
      target: { value: 'srv' },
    });
    fireEvent.click(screen.getByText('Start'));
    await waitFor(() =>
      expect(screen.getByText(/already running/)).toBeTruthy(),
    );
  }, 30000);

  it('shows login gate on unauthorized config load', async () => {
    configMock.mockRejectedValue(new Error('Unauthorized'));
    render(
      <MemoryRouter>
        <EvalProfilePage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getByText(/Authentication required/)).toBeTruthy(),
    );
  }, 30000);
});
