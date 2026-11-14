// [SHIELD_DEVCORE] Attach session: composes the data sources into one
// start/stop object for an IDE shell. Metrics come from the ops HTTP face
// when configured (primary), falling back to /proc sampling of the node
// process; logs always come from the file-tail poller (zero engine change).

import { EventEmitter } from 'node:events';

import { emitErrorSafe, toError } from './emitterUtil.js';
import { OpsHttpClient, processGauges, type PromSnapshot } from './sources/opsHttp.js';
import { LogTail, type LogRecord } from './sources/logTail.js';
import { LuaServicesSource, type LuaServiceEntry } from './sources/luaServices.js';
import { ProcSampler, type ProcSample } from './sources/procSampler.js';

export type SessionStatus =
  | 'detached'
  | 'attached'
  | 'degraded'
  | 'detaching';

export interface MetricsSample {
  at: number;
  source: 'ops-http' | 'proc';
  cpuPercent: number | null;
  rssBytes: number | null;
  threads: number | null;
  openFds: number | null;
  uptimeSeconds: number | null;
  serviceCount: number | null;
  /// Extra families worth charting later (kept opaque).
  extras?: PromSnapshot['families'];
}

export interface SessionOptions {
  ops?: {
    baseUrl: string;
    token?: string;
    timeoutMs?: number;
  };
  /// Fallback / supplemental process-level sampling (used when ops HTTP is
  /// absent, or always on when pid is given and opsPollMs unset).
  proc?: {
    pid: number;
    intervalMs?: number;
    procRoot?: string;
  };
  logFile?: {
    path: string;
    intervalMs?: number;
    fromEnd?: boolean;
  };
  /// Workspace roots used to resolve recorded Lua script paths.
  workspaceRoots?: readonly string[] | (() => readonly string[]);
  /// Poll cadence for the ops metrics face (default 2000ms, matching the
  /// console dashboard's baseline).
  opsPollMs?: number;
}

interface Events {
  status: (status: SessionStatus, detail?: string) => void;
  metrics: (sample: MetricsSample) => void;
  log: (record: LogRecord) => void;
  services: (entries: LuaServiceEntry[]) => void;
  error: (error: Error) => void;
}

export class ShieldSession extends EventEmitter {
  private readonly options: SessionOptions;
  private opsClient: OpsHttpClient | null = null;
  private opsTimer: NodeJS.Timeout | null = null;
  private procSampler: ProcSampler | null = null;
  private logTail: LogTail | null = null;
  private servicesSource: LuaServicesSource | null = null;
  private servicesTimer: NodeJS.Timeout | null = null;
  private statusValue: SessionStatus = 'detached';
  private running = false;

  constructor(options: SessionOptions) {
    super();
    this.options = options;
  }

  override on<K extends keyof Events>(event: K, listener: Events[K]): this {
    return super.on(event, listener);
  }

  override emit<K extends keyof Events>(event: K, ...args: Parameters<Events[K]>): boolean {
    return super.emit(event, ...args);
  }

  get status(): SessionStatus {
    return this.statusValue;
  }

  get ops(): OpsHttpClient | null {
    return this.opsClient;
  }

  private setStatus(status: SessionStatus, detail?: string): void {
    this.statusValue = status;
    this.emit('status', status, detail);
  }

  start(): void {
    if (this.running) {
      return;
    }
    this.running = true;

    let hasFace = false;

    if (this.options.ops) {
      hasFace = true;
      this.opsClient = new OpsHttpClient(this.options.ops);
      this.servicesSource = new LuaServicesSource(
        this.opsClient,
        this.options.workspaceRoots ?? [],
      );
      const opsPollMs = this.options.opsPollMs ?? 2000;
      const poll = async (): Promise<void> => {
        if (!this.opsClient) {
          return;
        }
        try {
          const snapshot = await this.opsClient.metrics();
          const gauges = processGauges(snapshot);
          this.emit('metrics', {
            at: Date.now(),
            source: 'ops-http',
            cpuPercent: gauges.cpuPercent,
            rssBytes: gauges.rssBytes,
            threads: gauges.threads,
            openFds: gauges.openFds,
            uptimeSeconds: gauges.uptimeSeconds,
            serviceCount: gauges.serviceCount,
          });
          if (this.statusValue !== 'attached') {
            this.setStatus('attached');
          }
        } catch (err) {
          if (this.statusValue !== 'degraded') {
            this.setStatus('degraded', 'ops http unreachable');
          }
          emitErrorSafe(this, toError(err));
        }
      };
      void poll();
      this.opsTimer = setInterval(() => void poll(), opsPollMs);
      this.opsTimer.unref?.();

      const refreshServices = async (): Promise<void> => {
        if (!this.servicesSource) {
          return;
        }
        try {
          this.emit('services', await this.servicesSource.refresh());
        } catch (err) {
          emitErrorSafe(this, toError(err));
        }
      };
      void refreshServices();
      this.servicesTimer = setInterval(() => void refreshServices(), opsPollMs * 5);
      this.servicesTimer.unref?.();
    }

    if (this.options.proc) {
      hasFace = true;
      // With ops HTTP active the proc sampler still runs when explicitly
      // requested (it is the fallback face for http.enabled=false nodes).
      this.procSampler = new ProcSampler({
        pid: this.options.proc.pid,
        intervalMs: this.options.proc.intervalMs ?? 1000,
        procRoot: this.options.proc.procRoot,
      });
      this.procSampler.on('sample', (sample: ProcSample) => {
        this.emit('metrics', {
          at: sample.at,
          source: 'proc',
          cpuPercent: sample.cpuPercent,
          rssBytes: sample.rssBytes,
          threads: sample.threads,
          openFds: sample.openFds,
          uptimeSeconds: null,
          serviceCount: null,
        });
      });
      this.procSampler.on('error', (err: Error) => emitErrorSafe(this, err));
      this.procSampler.start();
    }

    if (this.options.logFile) {
      hasFace = true;
      this.logTail = new LogTail({
        path: this.options.logFile.path,
        intervalMs: this.options.logFile.intervalMs ?? 500,
        fromEnd: this.options.logFile.fromEnd,
      });
      this.logTail.on('record', (record: LogRecord) => this.emit('log', record));
      this.logTail.on('rotated', () => this.emit('log', {
        timestampMs: Date.now(),
        level: 'info',
        logger: 'devcore',
        message: 'log file rotated; tail restarted',
        file: null,
        line: null,
        traceId: null,
        raw: '',
        continuation: false,
      }));
      this.logTail.on('error', (err: Error) => emitErrorSafe(this, err));
      this.logTail.start();
    }

    if (!hasFace) {
      this.setStatus('degraded', 'no data source configured');
    } else if (this.statusValue === 'detached') {
      // ops-less configurations (proc/log only) attach immediately.
      this.setStatus('attached');
    }
  }

  async detach(): Promise<void> {
    if (!this.running) {
      return;
    }
    this.setStatus('detaching');
    this.running = false;
    if (this.opsTimer) {
      clearInterval(this.opsTimer);
      this.opsTimer = null;
    }
    if (this.servicesTimer) {
      clearInterval(this.servicesTimer);
      this.servicesTimer = null;
    }
    this.procSampler?.stop();
    this.procSampler = null;
    this.logTail?.stop();
    this.logTail = null;
    this.opsClient = null;
    this.servicesSource = null;
    this.setStatus('detached');
  }
}
