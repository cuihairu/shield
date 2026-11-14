import { afterEach, describe, expect, it } from 'vitest';
import * as http from 'node:http';
import type { AddressInfo } from 'node:net';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { ShieldSession } from '../src/session.js';
import { resolveScriptPath } from '../src/sources/luaServices.js';

const servers: http.Server[] = [];
const dirs: string[] = [];
const sessions: ShieldSession[] = [];

afterEach(async () => {
  while (sessions.length > 0) {
    await sessions.pop()!.detach();
  }
  while (servers.length > 0) {
    const s = servers.pop()!;
    await new Promise<void>((resolve) => s.close(() => resolve()));
  }
  while (dirs.length > 0) {
    await rm(dirs.pop()!, { recursive: true, force: true });
  }
});

function startOps(handler: http.RequestListener): Promise<string> {
  const server = http.createServer(handler);
  servers.push(server);
  return new Promise<string>((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const addr = server.address() as AddressInfo;
      resolve(`http://127.0.0.1:${addr.port}`);
    });
  });
}

describe('resolveScriptPath', () => {
  it('keeps absolute script paths', () => {
    expect(resolveScriptPath('/srv/game/scripts/auth.lua', [])).toBe(
      '/srv/game/scripts/auth.lua',
    );
  });

  it('resolves a config-relative path against workspace roots', () => {
    expect(resolveScriptPath('../scripts/auth.lua', ['/srv/game/config'])).toBe(
      '/srv/game/scripts/auth.lua',
    );
  });

  it('returns null when no root can anchor a relative path', () => {
    expect(resolveScriptPath('../scripts/auth.lua', [])).toBeNull();
    expect(resolveScriptPath(undefined, ['/root'])).toBeNull();
    expect(resolveScriptPath('', ['/root'])).toBeNull();
  });
});

describe('ShieldSession', () => {
  it('streams metrics, services and logs from an attached node', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'shield-session-'));
    dirs.push(dir);
    const logPath = join(dir, 'shield.log');
    await writeFile(
      logPath,
      '1762748000001 [INFO] ops: HTTP ops server listening on 127.0.0.1:8080\n',
    );

    const base = await startOps((req, res) => {
      const respond = (data: unknown): void => {
        res.writeHead(200, { 'content-type': 'application/json' });
        res.end(JSON.stringify({ type: 'result', data }));
      };
      switch (req.url) {
        case '/ops/metrics':
          res.writeHead(200, { 'content-type': 'text/plain' });
          res.end(
            [
              '# TYPE shield_process_cpu_percent gauge',
              'shield_process_cpu_percent 4.2',
              '# TYPE shield_process_rss_bytes gauge',
              'shield_process_rss_bytes 209715200',
              '# TYPE shield_uptime_seconds gauge',
              'shield_uptime_seconds 10',
              '# TYPE shield_services gauge',
              'shield_services 1',
            ].join('\n'),
          );
          return;
        case '/ops/services':
          respond(['auth']);
          return;
        case '/ops/services/auth':
          respond({
            name: 'auth',
            state: 'running',
            script: '../scripts/auth.lua',
            rpc_routes: 2,
            requests: 5,
            errors: 0,
            memory_kb: 256,
            uptime_seconds: 9,
          });
          return;
        case '/ops/health':
          respond({ core: { status: 'ok' } });
          return;
        default:
          res.writeHead(404);
          res.end(JSON.stringify({ type: 'error', data: 'nf' }));
      }
    });

    const session = new ShieldSession({
      ops: { baseUrl: base },
      logFile: { path: logPath, intervalMs: 60, fromEnd: false },
      workspaceRoots: [join(dir, 'config')],
      opsPollMs: 100,
    });
    sessions.push(session);

    const statuses: string[] = [];
    session.on('status', (s) => statuses.push(s));
    const gotMetrics = new Promise<void>((resolve) =>
      session.on('metrics', (sample) => {
        if (sample.source === 'ops-http' && sample.cpuPercent === 4.2) {
          resolve();
        }
      }),
    );
    const gotServices = new Promise<void>((resolve) =>
      session.on('services', (entries) => {
        if (entries.some((e) => e.name === 'auth' && e.resolvedPath)) {
          resolve();
        }
      }),
    );
    const gotLog = new Promise<void>((resolve) =>
      session.on('log', (record) => {
        if (record.message.includes('HTTP ops server')) {
          resolve();
        }
      }),
    );

    session.start();
    await Promise.race([
      Promise.all([gotMetrics, gotServices, gotLog]),
      new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), 4000)),
    ]);
    await session.detach();
    expect(statuses).toContain('attached');
    expect(statuses[statuses.length - 1]).toBe('detached');
  }, 10000);

  it('flips to degraded when ops http is unreachable', async () => {
    // Grab a port and leave it closed.
    const probe = http.createServer(() => {});
    const port = await new Promise<number>((resolve) => {
      probe.listen(0, '127.0.0.1', () => {
        const addr = probe.address() as AddressInfo;
        resolve(addr.port);
      });
    });
    await new Promise<void>((resolve) => probe.close(() => resolve()));

    const session = new ShieldSession({
      ops: { baseUrl: `http://127.0.0.1:${port}`, timeoutMs: 400 },
      opsPollMs: 100,
    });
    sessions.push(session);

    const degraded = new Promise<void>((resolve) =>
      session.on('status', (s) => {
        if (s === 'degraded') {
          resolve();
        }
      }),
    );
    session.start();
    await Promise.race([
      degraded,
      new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), 4000)),
    ]);
    const errors: Error[] = [];
    session.on('error', (e) => errors.push(e));
    await session.detach();
  }, 10000);
});
