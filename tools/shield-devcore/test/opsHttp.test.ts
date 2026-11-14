import { afterEach, describe, expect, it } from 'vitest';
import * as http from 'node:http';
import type { AddressInfo } from 'node:net';

import {
  OpsHttpClient,
  OpsHttpError,
  parsePrometheusText,
  processGauges,
} from '../src/sources/opsHttp.js';

const PROFILE_REPORT = {
  type: 'result',
  data: {
    service: 'auth',
    duration_ms: 1000,
    interval: 10,
    total_samples: 100,
    frames: [
      {
        what: 'function',
        name: 'login',
        source: 'auth.lua',
        line: 20,
        tail: false,
        hits: 40,
        pct: 40.0,
        children: [
          {
            what: 'function',
            name: 'shield.client.bind',
            source: '=builtin',
            line: 0,
            tail: true,
            hits: 12,
            pct: 12.0,
            children: [],
          },
        ],
      },
    ],
  },
};

const PROM_TEXT = [
  '# HELP shield_process_rss_bytes Resident set size in bytes.',
  '# TYPE shield_process_rss_bytes gauge',
  'shield_process_rss_bytes 104857600',
  '# TYPE shield_process_cpu_percent gauge',
  'shield_process_cpu_percent 12.5',
  '# TYPE shield_uptime_seconds gauge',
  'shield_uptime_seconds 42',
  '# TYPE shield_services gauge',
  'shield_services 3',
  '# TYPE shield_service_requests_total counter',
  'shield_service_requests_total{service="auth"} 100',
  'shield_service_requests_total{service="room_1"} 250',
].join('\n');

const servers: http.Server[] = [];
afterEach(async () => {
  while (servers.length > 0) {
    const s = servers.pop()!;
    await new Promise<void>((resolve) => s.close(() => resolve()));
  }
});

function startOps(handler: http.RequestListener): Promise<string> {
  const server = http.createServer(handler);
  servers.push(server);
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const addr = server.address() as AddressInfo;
      resolve(`http://127.0.0.1:${addr.port}`);
    });
  });
}

describe('parsePrometheusText', () => {
  it('parses families, types, help and labelled samples', () => {
    const snap = parsePrometheusText(PROM_TEXT);
    expect(snap.byName.get('shield_process_rss_bytes')?.value).toBe(104857600);
    expect(snap.byName.get('shield_process_cpu_percent')?.value).toBe(12.5);
    const family = snap.families.get('shield_service_requests_total');
    expect(family?.type).toBe('counter');
    expect(family?.samples).toHaveLength(2);
    expect(family?.samples[0].labels).toEqual({ service: 'auth' });
    expect(family?.samples[1].labels).toEqual({ service: 'room_1' });
  });

  it('keeps label values containing spaces and braces', () => {
    const snap = parsePrometheusText('x_total{msg="a b {c}"} 1\n');
    const sample = snap.byName.get('x_total{msg="a b {c}"}');
    expect(sample?.value).toBe(1);
    expect(sample?.labels['msg']).toBe('a b {c}');
  });

  it('ignores blank and unknown comment lines', () => {
    const snap = parsePrometheusText('\n# just a note\n\n  \n');
    expect(snap.families.size).toBe(0);
  });

  it('processGauges pulls the process families', () => {
    const gauges = processGauges(parsePrometheusText(PROM_TEXT));
    expect(gauges.cpuPercent).toBe(12.5);
    expect(gauges.rssBytes).toBe(104857600);
    expect(gauges.uptimeSeconds).toBe(42);
    expect(gauges.serviceCount).toBe(3);
    expect(gauges.threads).toBeNull();
  });
});

describe('OpsHttpClient', () => {
  it('sends the bearer token to gated endpoints', async () => {
    const seen: { path: string; auth: string }[] = [];
    const base = await startOps((req, res) => {
      seen.push({ path: req.url ?? '', auth: req.headers.authorization ?? '' });
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ type: 'result', data: [] }));
    });
    const client = new OpsHttpClient({ baseUrl: base, token: 'sekrit' });
    await client.services();
    expect(seen[0].auth).toBe('Bearer sekrit');
  });

  it('parses the services envelope', async () => {
    const base = await startOps((_req, res) => {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ type: 'result', data: ['auth', 'player', 'room'] }));
    });
    const client = new OpsHttpClient({ baseUrl: base });
    expect(await client.services()).toEqual(['auth', 'player', 'room']);
  });

  it('tolerates the engine timeout sentinel (data:"timeout")', async () => {
    const base = await startOps((_req, res) => {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ type: 'result', data: 'timeout' }));
    });
    const client = new OpsHttpClient({ baseUrl: base });
    expect(await client.services()).toEqual([]);
  });

  it('parses metrics text', async () => {
    const base = await startOps((req, res) => {
      if (req.url === '/ops/metrics') {
        res.writeHead(200, { 'content-type': 'text/plain; version=0.0.4; charset=utf-8' });
        res.end(PROM_TEXT);
        return;
      }
      res.writeHead(404, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ type: 'error', data: 'not found' }));
    });
    const client = new OpsHttpClient({ baseUrl: base });
    const snap = await client.metrics();
    expect(processGauges(snap).rssBytes).toBe(104857600);
  });

  it('maps the service detail JSON to camelCase fields', async () => {
    const base = await startOps((req, res) => {
      if (req.url === '/ops/services/auth') {
        res.writeHead(200, { 'content-type': 'application/json' });
        res.end(
          JSON.stringify({
            type: 'result',
            data: {
              name: 'auth',
              state: 'running',
              script: '../scripts/auth.lua',
              rpc_routes: 2,
              requests: 10,
              errors: 1,
              uptime_seconds: 55,
              timers: 0,
              pending_calls: 0,
              pending_tasks: 0,
              coroutines: 2,
              memory_kb: 512,
            },
          }),
        );
        return;
      }
      res.writeHead(404, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ type: 'error', data: 'nf' }));
    });
    const client = new OpsHttpClient({ baseUrl: base });
    const detail = await client.serviceDetail('auth');
    expect(detail).not.toBeNull();
    expect(detail!.script).toBe('../scripts/auth.lua');
    expect(detail!.rpcRoutes).toBe(2);
    expect(detail!.memoryKb).toBe(512);
    expect(await client.serviceDetail('missing')).toBeNull();
  });

  it('reports unauthorized as an OpsHttpError with status 401', async () => {
    const base = await startOps((_req, res) => {
      res.writeHead(401, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ type: 'error', data: 'unauthorized' }));
    });
    const client = new OpsHttpClient({ baseUrl: base, token: 'wrong' });
    await expect(client.metrics()).rejects.toBeInstanceOf(OpsHttpError);
    try {
      await client.metrics();
    } catch (err) {
      expect((err as OpsHttpError).status).toBe(401);
    }
  });

  it('derives health ok from the top-level status (live engine shape)', async () => {
    const base = await startOps((_req, res) => {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(
        JSON.stringify({
          type: 'result',
          data: {
            checks: {
              core: { status: 'ok', uptime_seconds: 9.48 },
              plugins: { status: 'ok', started: 0, required_down: 0 },
              server: { status: 'ok', state: 'running' },
            },
            status: 'ok',
            uptime: 9.48,
          },
        }),
      );
    });
    const client = new OpsHttpClient({ baseUrl: base });
    const health = await client.health();
    expect(health.ok).toBe(true);

    // degraded top-level status flips ok off
    const degradedBase = await startOps((_req, res) => {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(
        JSON.stringify({
          type: 'result',
          data: {
            checks: { plugins: { status: 'degraded' } },
            status: 'degraded',
          },
        }),
      );
    });
    const degraded = new OpsHttpClient({ baseUrl: degradedBase });
    expect((await degraded.health()).ok).toBe(false);
  });

  it('falls back to the per-check scan when no top-level status', async () => {
    const base = await startOps((_req, res) => {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(
        JSON.stringify({
          type: 'result',
          data: {
            core: { status: 'ok', uptime_seconds: 3 },
            plugins: { status: 'ok', started: 0, required_down: 0 },
          },
        }),
      );
    });
    const client = new OpsHttpClient({ baseUrl: base });
    const health = await client.health();
    expect(health.ok).toBe(true);
  });

  it('drives the profile sampler with the separate profile token', async () => {
    const seenBodies: unknown[] = [];
    const seenAuth: string[] = [];
    const base = await startOps((req, res) => {
      seenAuth.push(req.headers.authorization ?? '');
      let raw = '';
      req.on('data', (c) => (raw += c));
      req.on('end', () => {
        seenBodies.push(JSON.parse(raw || '{}'));
        if (seenBodies.length === 1) {
          res.writeHead(200, { 'content-type': 'application/json' });
          res.end(JSON.stringify({ type: 'result', data: { started: true, service: 'auth' } }));
        } else {
          res.writeHead(200, { 'content-type': 'application/json' });
          res.end(JSON.stringify(PROFILE_REPORT));
        }
      });
    });
    // duration 0 -> sampleOnce waits ~300ms only.
    const client = new OpsHttpClient({
      baseUrl: base,
      token: 'ops-token',
      profileToken: 'profile-token',
      timeoutMs: 5000,
    });
    const report = await client.sampleOnce('auth', 0);
    expect(report.total_samples).toBe(100);
    expect(report.frames?.[0].name).toBe('login');
    expect(report.frames?.[0].children[0].pct).toBe(12.0);
    // The start request carried the PROFILE token, not the ops token.
    expect(seenBodies[0]).toEqual({ action: 'start', service: 'auth', duration_ms: 0 });
    expect(seenAuth[0]).toBe('Bearer profile-token');
    expect(seenAuth[1]).toBe('Bearer profile-token');
  });
});
