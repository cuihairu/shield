// [SHIELD_DEVCORE] Minimal read-mostly TCP transport client speaking the
// shield wire format (see src/protocol/envelope.ts). It connects to a
// gateway/listener, feeds every byte into the configured envelope decoder
// and re-emits decoded packets; dropped connections are retried with
// exponential backoff + jitter until stop() is called.

import { EventEmitter } from 'node:events';
import type { Socket } from 'node:net';
import { connect } from 'node:net';

import { EnvelopeDecoder, type EnvelopeConfig, type Packet } from '../protocol/envelope.js';

export type TransportStatus =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'waiting-retry'
  | 'stopped';

export interface TcpTransportOptions {
  host: string;
  port: number;
  /// Wire framing of the target listener (engine network.protocol.envelope).
  envelope: EnvelopeConfig;
  /// Backoff base in ms (default 250). Actual delay = base * 2^attempt + jitter.
  backoffBaseMs?: number;
  /// Backoff cap in ms (default 8000).
  backoffMaxMs?: number;
  /// Connection timeout in ms (default 5000).
  connectTimeoutMs?: number;
}

interface Events {
  packet: (packet: Packet) => void;
  status: (status: TransportStatus, detail?: string) => void;
  'protocol-error': (message: string) => void;
}

export class ShieldTcpClient extends EventEmitter {
  private readonly options: Required<
    Pick<TcpTransportOptions, 'host' | 'port' | 'backoffBaseMs' | 'backoffMaxMs' | 'connectTimeoutMs'>
  > & { envelope: EnvelopeConfig };
  private decoder: EnvelopeDecoder;
  private socket: Socket | null = null;
  private retryTimer: NodeJS.Timeout | null = null;
  private connectionTimer: NodeJS.Timeout | null = null;
  private attempt = 0;
  private stopped = false;
  private currentStatus: TransportStatus = 'idle';

  constructor(options: TcpTransportOptions) {
    super();
    this.options = {
      host: options.host,
      port: options.port,
      envelope: options.envelope,
      backoffBaseMs: options.backoffBaseMs ?? 250,
      backoffMaxMs: options.backoffMaxMs ?? 8000,
      connectTimeoutMs: options.connectTimeoutMs ?? 5000,
    };
    this.decoder = new EnvelopeDecoder(options.envelope);
  }

  override on<K extends keyof Events>(event: K, listener: Events[K]): this {
    return super.on(event, listener);
  }

  override emit<K extends keyof Events>(event: K, ...args: Parameters<Events[K]>): boolean {
    return super.emit(event, ...args);
  }

  get status(): TransportStatus {
    return this.currentStatus;
  }

  get decoderError(): string {
    return this.decoder.error;
  }

  start(): void {
    if (this.stopped) {
      return;
    }
    this.stopped = false;
    this.attempt = 0;
    this.openConnection();
  }

  stop(): void {
    this.stopped = true;
    this.clearRetryTimer();
    this.clearConnectionTimer();
    this.destroySocket();
    this.setStatus('stopped');
  }

  /// Forget any partial frame state (used after a protocol error so a fresh
  /// connection starts decoding clean).
  resetDecoder(): void {
    this.decoder.reset();
  }

  private setStatus(status: TransportStatus, detail?: string): void {
    if (this.currentStatus !== status || detail !== undefined) {
      this.currentStatus = status;
      this.emit('status', status, detail);
    }
  }

  private clearRetryTimer(): void {
    if (this.retryTimer) {
      clearTimeout(this.retryTimer);
      this.retryTimer = null;
    }
  }

  private clearConnectionTimer(): void {
    if (this.connectionTimer) {
      clearTimeout(this.connectionTimer);
      this.connectionTimer = null;
    }
  }

  private destroySocket(): void {
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = null;
    }
  }

  private scheduleRetry(): void {
    // 'error' and 'close' (and the connect timeout) can all fire for one
    // dropped attempt; only the first schedules, the pending timer is the
    // single retry authority.
    if (this.stopped || this.retryTimer) {
      return;
    }
    const exp = Math.min(
      this.options.backoffBaseMs * 2 ** this.attempt,
      this.options.backoffMaxMs,
    );
    // Full jitter over [exp/2, exp] keeps reconnect storms off the node.
    const delay = exp / 2 + Math.random() * (exp / 2);
    this.attempt += 1;
    this.setStatus('waiting-retry', `retry in ${Math.round(delay)}ms`);
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this.openConnection();
    }, delay);
    this.retryTimer.unref?.();
  }

  private openConnection(): void {
    if (this.stopped) {
      return;
    }
    this.clearRetryTimer();
    this.clearConnectionTimer();
    this.destroySocket();
    this.setStatus('connecting');

    const socket = connect({ host: this.options.host, port: this.options.port });
    this.socket = socket;

    socket.setNoDelay(true);

    this.connectionTimer = setTimeout(() => {
      socket.destroy();
      this.connectionTimer = null;
      this.scheduleRetry();
    }, this.options.connectTimeoutMs);
    this.connectionTimer.unref?.();

    socket.on('connect', () => {
      this.clearConnectionTimer();
      this.attempt = 0;
      this.setStatus('connected');
    });

    socket.on('data', (chunk: Buffer) => {
      if (this.decoder.hasError) {
        return;
      }
      for (const packet of this.decoder.feed(chunk)) {
        this.emit('packet', packet);
      }
      if (this.decoder.hasError) {
        this.emit('protocol-error', this.decoder.error);
      }
    });

    socket.on('error', (err: Error) => {
      this.clearConnectionTimer();
      if (socket === this.socket) {
        this.destroySocket();
      }
      this.scheduleRetry();
      this.emit('status', 'waiting-retry', err.message);
    });

    socket.on('close', () => {
      if (this.stopped) {
        return;
      }
      this.clearConnectionTimer();
      if (socket === this.socket) {
        this.destroySocket();
        this.scheduleRetry();
      }
    });
  }
}
