import { afterEach, describe, expect, it } from 'vitest';
import * as net from 'node:net';

import { encodePacket } from '../src/protocol/envelope.js';
import { ShieldTcpClient, type TransportStatus } from '../src/transport/tcpClient.js';

const IDLEN = { kind: 'idlen' as const };

function frame(routeId: number, body: string): Buffer {
  return encodePacket({ routeId, kind: 0, flags: 0, seq: 0, body: Buffer.from(body) }, IDLEN);
}

interface ServerHandle {
  server: net.Server;
  port: number;
  close(): Promise<void>;
  connections(): Promise<net.Socket[]>;
}

async function listen(handler?: (socket: net.Socket) => void): Promise<ServerHandle> {
  const sockets: net.Socket[] = [];
  const server = net.createServer((socket) => {
    sockets.push(socket);
    handler?.(socket);
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const addr = server.address();
  if (typeof addr !== 'object' || addr === null) {
    throw new Error('no port');
  }
  return {
    server,
    port: addr.port,
    async close(): Promise<void> {
      for (const s of sockets) {
        s.destroy();
      }
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
    async connections(): Promise<net.Socket[]> {
      return sockets;
    },
  };
}

const active: ServerHandle[] = [];
afterEach(async () => {
  while (active.length > 0) {
    await active.pop()?.close();
  }
});

describe('ShieldTcpClient', () => {
  it('decodes frames pushed by a server', async () => {
    const handle = await listen();
    active.push(handle);

    const client = new ShieldTcpClient({
      host: '127.0.0.1',
      port: handle.port,
      envelope: IDLEN,
      backoffBaseMs: 10,
    });
    const packets: { routeId: number; body: string }[] = [];
    client.on('packet', (p) => packets.push({ routeId: p.routeId, body: p.body.toString() }));
    const statusSeen: TransportStatus[] = [];
    client.on('status', (s) => statusSeen.push(s));
    client.start();

    // wait for the client connection
    while ((await handle.connections()).length === 0) {
      await new Promise((r) => setTimeout(r, 10));
    }
    const sock = (await handle.connections())[0];
    sock.write(frame(1, 'hello'));
    sock.write(frame(2, 'second frame'));

    await new Promise((r) => setTimeout(r, 50));
    client.stop();
    expect(packets).toContainEqual({ routeId: 1, body: 'hello' });
    expect(statusSeen).toContain('connected');
    expect(client.status).toBe('stopped');
  });

  it('reconnects with backoff after a server drop', async () => {
    let drops = 0;
    const handle = await listen((socket) => {
      // Server policy: any accepted connection is dropped immediately.
      socket.on('close', () => {
        drops += 1;
      });
      socket.destroy();
    });
    active.push(handle);

    const client = new ShieldTcpClient({
      host: '127.0.0.1',
      port: handle.port,
      envelope: IDLEN,
      backoffBaseMs: 20,
      backoffMaxMs: 200,
      connectTimeoutMs: 500,
    });
    let connects = 0;
    client.on('status', (s) => {
      if (s === 'connected') {
        connects += 1;
      }
    });
    client.start();

    // Wait for at least one drop plus one successful reconnect.
    const deadline = Date.now() + 5000;
    while (connects < 2 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 25));
    }
    client.stop();
    expect(drops).toBeGreaterThanOrEqual(1);
    expect(connects).toBeGreaterThanOrEqual(2);
  }, 15000);

  it('keeps retrying when the port is not listening', async () => {
    // Grab a free port then release it: nothing listens there now.
    const probe = await listen();
    const port = probe.port;
    await probe.close();

    const client = new ShieldTcpClient({
      host: '127.0.0.1',
      port,
      envelope: IDLEN,
      backoffBaseMs: 20,
      backoffMaxMs: 100,
      connectTimeoutMs: 200,
    });
    let waiting = 0;
    let connectedSeen = false;
    client.on('status', (s) => {
      if (s === 'waiting-retry') {
        waiting += 1;
      } else if (s === 'connected') {
        connectedSeen = true;
      }
    });
    client.start();

    // Now start a server on that port: the next retry must connect.
    const server = net.createServer((socket) => socket.destroy());
    active.push({
      server,
      port,
      async close(): Promise<void> {
        await new Promise<void>((resolve) => server.close(() => resolve()));
      },
      async connections(): Promise<net.Socket[]> {
        return [];
      },
    });
    await new Promise<void>((resolve, reject) => {
      server.once('error', reject);
      server.listen(port, '127.0.0.1', () => resolve());
    });

    const deadline = Date.now() + 5000;
    while (!connectedSeen && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    client.stop();
    expect(waiting).toBeGreaterThanOrEqual(1);
    expect(connectedSeen).toBe(true);
  }, 15000);

  it('surfaces a protocol error once and stops decoding until reset', async () => {
    const handle = await listen();
    active.push(handle);

    const client = new ShieldTcpClient({
      host: '127.0.0.1',
      port: handle.port,
      envelope: { kind: 'idlen', maxFrameSize: 1024 },
      backoffBaseMs: 10,
    });
    const errors: string[] = [];
    client.on('protocol-error', (m) => errors.push(m));
    client.start();
    while ((await handle.connections()).length === 0) {
      await new Promise((r) => setTimeout(r, 10));
    }
    const sock = (await handle.connections())[0];
    // length field 4 bytes wide configured? idlen default length is 2 bytes;
    // send a length that exceeds the cap by writing huge value.
    const evil = Buffer.alloc(4);
    evil.writeUInt16BE(0xffff, 0);
    evil.writeUInt16BE(0xffff, 2);
    sock.write(evil);

    await new Promise((r) => setTimeout(r, 50));
    client.stop();
    expect(errors).toHaveLength(1);
    expect(client.decoderError).toContain('too large');
  });
});
