import { describe, expect, it, vi, beforeEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import PluginsPage from '../PluginsPage';
import { api } from '../../services/api';

vi.mock('../../services/api', () => ({
  api: {
    plugins: vi.fn(),
    metricsText: vi.fn(),
  },
}));

describe('PluginsPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('shows degraded warning when a required plugin is not started', async () => {
    (api.plugins as ReturnType<typeof vi.fn>).mockResolvedValue([
      { id: 'db.default', package: 'sqlite', state: 'failed', required: true },
    ]);
    (api.metricsText as ReturnType<typeof vi.fn>).mockResolvedValue('');
    render(
      <MemoryRouter>
        <PluginsPage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getByText(/Required plugin degraded/)).toBeTruthy(),
    );
  });

  it('reconciles pool metrics from the same /ops/metrics source', async () => {
    (api.plugins as ReturnType<typeof vi.fn>).mockResolvedValue([
      { id: 'db.default', package: 'sqlite', state: 'started', required: true },
    ]);
    (api.metricsText as ReturnType<typeof vi.fn>).mockResolvedValue(
      '# TYPE shield_plugin_pool_size gauge\n' +
        'shield_plugin_pool_size{plugin="db",instance="db.default"} 10\n' +
        'shield_plugin_pool_in_use{plugin="db",instance="db.default"} 3\n' +
        'shield_plugin_pool_idle{plugin="db",instance="db.default"} 7\n',
    );
    render(
      <MemoryRouter>
        <PluginsPage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getAllByText('db.default').length).toBeGreaterThan(0),
    );
    expect(screen.getAllByText('10').length).toBeGreaterThan(0);
    expect(screen.getAllByText('3').length).toBeGreaterThan(0);
    expect(screen.getAllByText('7').length).toBeGreaterThan(0);
  });
});
