// [SHIELD_DEVCORE] Process-level CPU/RSS sampling for nodes that do not
// expose the ops HTTP face (http.enabled=false). Linux reads
// /proc/<pid>/stat, /proc/<pid>/status and counts /proc/<pid>/fd entries;
// the CPU gauge is a rate over the poll interval (same convention as the
// engine's /ops/metrics shield_process_cpu_percent). The parse helpers are
// pure so tests can feed fixture file contents.

import { EventEmitter } from 'node:events';
import { readFile, readdir } from 'node:fs/promises';

import { emitErrorSafe, toError } from '../emitterUtil.js';

/// /proc/<pid>/stat fields 14 (utime) and 15 (stime) in clock ticks.
export interface ProcStatCpu {
  utimeTicks: number;
  stimeTicks: number;
}

/// Fields 14/15 come after the parenthesised comm field, which may contain
/// spaces and parens, so scan from the last ')' instead of splitting.
export function parseProcStat(content: string): ProcStatCpu | null {
  const closeParen = content.lastIndexOf(')');
  if (closeParen < 0) {
    return null;
  }
  const fields = content.slice(closeParen + 1).trim().split(/\s+/);
  // After removing "pid (comm)", the remaining field list starts at state
  // (field 3), so utime (14) is index 14 - 3 = 11 and stime (15) is 12.
  const utime = Number(fields[11]);
  const stime = Number(fields[12]);
  if (!Number.isFinite(utime) || !Number.isFinite(stime)) {
    return null;
  }
  return { utimeTicks: utime, stimeTicks: stime };
}

export interface ProcStatus {
  vmRssBytes: number | null;
  threads: number | null;
}

export function parseProcStatus(content: string): ProcStatus {
  const kb = content.match(/^VmRSS:\s+(\d+)\s+kB$/m);
  const threads = content.match(/^Threads:\s+(\d+)$/m);
  return {
    vmRssBytes: kb ? Number(kb[1]) * 1024 : null,
    threads: threads ? Number(threads[1]) : null,
  };
}

export async function countOpenFds(procDir: string): Promise<number | null> {
  try {
    const entries = await readdir(procDir);
    return entries.length;
  } catch {
    return null;
  }
}

export interface ProcSample {
  at: number;
  cpuSeconds: number | null;
  cpuPercent: number | null;
  rssBytes: number | null;
  threads: number | null;
  openFds: number | null;
}

export interface ProcSamplerOptions {
  pid: number;
  procRoot?: string;
  /// Clock ticks per second (kernel CLK_TCK; 100 on stock Linux).
  ticksPerSecond?: number;
  intervalMs?: number;
}

function readOrNil(path: string): Promise<string | null> {
  return readFile(path, 'utf8').catch(() => null);
}

export async function readProcSample(options: {
  pid: number;
  procRoot?: string;
  ticksPerSecond?: number;
  previous?: ProcSample | null;
}): Promise<ProcSample> {
  const procRoot = options.procRoot ?? '/proc';
  const ticks = options.ticksPerSecond ?? 100;
  const dir = `${procRoot}/${options.pid}`;
  const [stat, status] = await Promise.all([
    readOrNil(`${dir}/stat`),
    readOrNil(`${dir}/status`),
  ]);

  const at = Date.now();
  let cpuSeconds: number | null = null;
  let cpuPercent: number | null = null;
  if (stat) {
    const cpu = parseProcStat(stat);
    if (cpu) {
      cpuSeconds = (cpu.utimeTicks + cpu.stimeTicks) / ticks;
      if (options.previous?.cpuSeconds != null) {
        const dt = (at - options.previous.at) / 1000;
        if (dt > 0) {
          cpuPercent = ((cpuSeconds - options.previous.cpuSeconds) / dt) * 100;
        }
      } else {
        // First sample: 0, mirroring the engine's gauge convention.
        cpuPercent = 0;
      }
    }
  }

  let rssBytes: number | null = null;
  let threads: number | null = null;
  if (status) {
    const parsed = parseProcStatus(status);
    rssBytes = parsed.vmRssBytes;
    threads = parsed.threads;
  }
  const openFds = await countOpenFds(`${dir}/fd`);

  return { at, cpuSeconds, cpuPercent, rssBytes, threads, openFds };
}

interface Events {
  sample: (sample: ProcSample) => void;
  error: (error: Error) => void;
}

/// Interval poller emitting ProcSample events until stop().
export class ProcSampler extends EventEmitter {
  private readonly options: Required<Omit<ProcSamplerOptions, 'procRoot'>> & {
    procRoot?: string;
  };
  private timer: NodeJS.Timeout | null = null;
  private previous: ProcSample | null = null;
  private running = false;

  constructor(options: ProcSamplerOptions) {
    super();
    this.options = {
      pid: options.pid,
      procRoot: options.procRoot,
      ticksPerSecond: options.ticksPerSecond ?? 100,
      intervalMs: options.intervalMs ?? 1000,
    };
  }

  override on<K extends keyof Events>(event: K, listener: Events[K]): this {
    return super.on(event, listener);
  }

  override emit<K extends keyof Events>(event: K, ...args: Parameters<Events[K]>): boolean {
    return super.emit(event, ...args);
  }

  start(): void {
    if (this.running) {
      return;
    }
    this.running = true;
    const tick = async (): Promise<void> => {
      if (!this.running) {
        return;
      }
      try {
        const sample = await readProcSample({
          pid: this.options.pid,
          procRoot: this.options.procRoot,
          ticksPerSecond: this.options.ticksPerSecond,
          previous: this.previous,
        });
        this.previous = sample;
        this.emit('sample', sample);
      } catch (err) {
        emitErrorSafe(this, toError(err));
      }
      if (this.running) {
        this.timer = setTimeout(tick, this.options.intervalMs);
        this.timer.unref?.();
      }
    };
    void tick();
  }

  stop(): void {
    this.running = false;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }
}
