import { describe, expect, it, vi, beforeEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import ServicesPage from '../ServicesPage';
import { api } from '../../services/api';

vi.mock('../../services/api', () => ({
  api: {
    services: vi.fn(),
    serviceDetail: vi.fn(),
  },
}));

describe('ServicesPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('shows explicit unavailable state on list timeout, not an empty table', async () => {
    (api.services as ReturnType<typeof vi.fn>).mockRejectedValue(
      new Error('timeout querying services'),
    );
    render(
      <MemoryRouter>
        <ServicesPage />
      </MemoryRouter>,
    );
    await waitFor(() =>
      expect(screen.getByText(/Services unavailable/)).toBeTruthy(),
    );
    expect(screen.queryByText(/No services running/)).toBeNull();
  });

  it('distinguishes 404 not-found from 504 timeout in detail', async () => {
    (api.services as ReturnType<typeof vi.fn>).mockResolvedValue(['srv1']);
    (api.serviceDetail as ReturnType<typeof vi.fn>).mockRejectedValue(
      new Error('service not found: srv1'),
    );
    render(
      <MemoryRouter>
        <ServicesPage />
      </MemoryRouter>,
    );
    await waitFor(() => expect(screen.getByText('srv1')).toBeTruthy());
    // Click the service link to open the detail.
    (screen.getByText('srv1') as HTMLAnchorElement).click();
    await waitFor(() =>
      expect(screen.getByText(/Service not found \(404\)/)).toBeTruthy(),
    );
  });
});
