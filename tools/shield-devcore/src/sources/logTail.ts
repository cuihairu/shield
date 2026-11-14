// [SHIELD_DEVCORE] File-tail log source (phase one of the log face: plain
// polling of the node's log file — zero engine change; the socket tee is
// deferred). The engine writes lines via FileSink/RotatingFileSink in the
// format produced by format_record() (src/log/logger.cpp):
//   <timestamp_ms> [LEVEL] logger: message (file:line) trace=<id>
// Rotation/truncation is detected by the file shrinking below the stored
// offset; the tailer then restarts from the beginning.

import { EventEmitter } from 'node:events';
import { open, stat } from 'node:fs/promises';

import { emitErrorSafe, toError } from '../emitterUtil.js';

export type LogLevel = 'debug' | 'info' | 'warning' | 'error' | 'fatal' | 'unknown';

export interface LogRecord {
  timestampMs: number | null;
  level: LogLevel;
  logger: string;
  message: string;
  file: string | null;
  line: number | null;
  traceId: string | null;
  /// Original line, always present (continuation lines only carry this).
  raw: string;
  /// True when the line did not match the engine format and was treated as
  /// a continuation of the previous record (multi-line messages).
  continuation: boolean;
}

const LEVELS: ReadonlySet<string> = new Set([
  'DEBUG',
  'INFO',
  'WARNING',
  'ERROR',
  'FATAL',
]);

function levelFromName(name: string): LogLevel {
  switch (name) {
    case 'DEBUG':
      return 'debug';
    case 'INFO':
      return 'info';
    case 'WARNING':
      return 'warning';
    case 'ERROR':
      return 'error';
    case 'FATAL':
      return 'fatal';
    default:
      return 'unknown';
  }
}

/// Parse one engine log line. Pure function. Returns null for lines that
/// should be treated as continuations of the previous record.
export function parseShieldLogLine(line: string): LogRecord | null {
  // <ts> [LEVEL] logger: message...
  const match = line.match(/^(\d+) \[([A-Z]+)\] ([^:\s]+): (.*)$/);
  if (!match) {
    return null;
  }
  const [, tsStr, levelName, logger, rest] = match;
  let message = rest;
  let traceId: string | null = null;
  let file: string | null = null;
  let lineNo: number | null = null;

  // Trailing trace=<id>
  const traceMatch = message.match(/\s+trace=([^\s]+)$/);
  if (traceMatch) {
    traceId = traceMatch[1];
    message = message.slice(0, message.length - traceMatch[0].length);
  }
  // Trailing (file:line)
  const locMatch = message.match(/\s+\(([^()]+):(\d+)\)$/);
  if (locMatch) {
    file = locMatch[1];
    lineNo = Number(locMatch[2]);
    message = message.slice(0, message.length - locMatch[0].length);
  }

  return {
    timestampMs: Number(tsStr),
    level: LEVELS.has(levelName) ? levelFromName(levelName) : 'unknown',
    logger,
    message,
    file,
    line: lineNo,
    traceId,
    raw: line,
    continuation: false,
  };
}

/// Wrap continuation lines into a synthetic record carrying the raw text
/// (the webview renders them attached to the previous entry).
export function continuationRecord(line: string): LogRecord {
  return {
    timestampMs: null,
    level: 'unknown',
    logger: '',
    message: line,
    file: null,
    line: null,
    traceId: null,
    raw: line,
    continuation: true,
  };
}

interface Events {
  record: (record: LogRecord) => void;
  rotated: () => void;
  error: (error: Error) => void;
}

export interface LogTailOptions {
  path: string;
  intervalMs?: number;
  /// Start from the end of an existing file (like `tail -f`) instead of
  /// replaying history (default true).
  fromEnd?: boolean;
  maxChunkBytes?: number;
}

/// Poll the node's log file and emit parsed records. Never throws on a
/// missing file: it waits for the file to appear.
export class LogTail extends EventEmitter {
  private readonly path: string;
  private readonly intervalMs: number;
  private readonly maxChunkBytes: number;
  private offset: number;
  private lastIno: number | null = null;
  private remainder = '';
  private timer: NodeJS.Timeout | null = null;
  private running = false;
  private polling = false;

  constructor(options: LogTailOptions) {
    super();
    this.path = options.path;
    this.intervalMs = options.intervalMs ?? 500;
    this.maxChunkBytes = options.maxChunkBytes ?? 4 * 1024 * 1024;
    this.offset = options.fromEnd === false ? 0 : -1; // -1 = seek to end on first sight
  }

  override on<K extends keyof Events>(event: K, listener: Events[K]): this {
    return super.on(event, listener);
  }

  override emit<K extends keyof Events>(event: K, ...args: Parameters<Events[K]>): boolean {
    return super.emit(event, ...args);
  }

  get file(): string {
    return this.path;
  }

  start(): void {
    if (this.running) {
      return;
    }
    this.running = true;
    const tick = (): void => {
      if (!this.running || this.polling) {
        return;
      }
      this.polling = true;
      this.poll()
        .catch((err) => emitErrorSafe(this, toError(err)))
        .finally(() => {
          this.polling = false;
          if (this.running) {
            this.timer = setTimeout(tick, this.intervalMs);
            this.timer.unref?.();
          }
        });
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

  /// Read everything new since the last poll and emit records. Public for
  /// tests that drive the tailer without timers.
  async poll(): Promise<number> {
    let info;
    try {
      info = await stat(this.path);
    } catch {
      return 0; // file not created yet
    }
    if (!info.isFile()) {
      return 0;
    }

    const size = info.size;
    // Rotation detection: RotatingFileSink renames base.N and recreates the
    // base file fresh. Two independent signals: the inode changed (the file
    // is a different one, even when the new file is already larger than our
    // offset), or the file shrank below the read offset (truncation).
    const inoChanged =
      this.lastIno !== null && info.ino !== 0 && this.lastIno !== 0 && info.ino !== this.lastIno;
    if (inoChanged || size < this.offset) {
      this.offset = 0;
      this.remainder = '';
      this.emit('rotated');
    }
    if (info.ino !== 0) {
      this.lastIno = info.ino;
    }

    if (this.offset === -1) {
      // First sighting of an existing file: start from the end (tail -f
      // semantics); history replay is opt-in via fromEnd=false.
      this.offset = size;
      return 0;
    }
    if (size === this.offset) {
      return 0;
    }

    const readLen = Math.min(size - this.offset, this.maxChunkBytes);
    const handle = await open(this.path, 'r');
    try {
      const buf = Buffer.alloc(readLen);
      const { bytesRead } = await handle.read(buf, 0, readLen, this.offset);
      this.offset += bytesRead;
      // bytesRead < readLen means the poll consumed everything the file had
      // (no more data could complete a partial line before the next poll).
      this.emitLines(buf.subarray(0, bytesRead), bytesRead < readLen);
      return bytesRead;
    } finally {
      await handle.close();
    }
  }

  private emitLines(chunk: Buffer, atEof: boolean): void {
    let text = this.remainder + chunk.toString('utf8');
    // A very long line beyond maxChunkBytes could keep the buffer growing;
    // cap the buffer to something sane and drop the front.
    if (text.length > this.maxChunkBytes) {
      text = text.slice(-this.maxChunkBytes);
    }

    const lastNewline = text.lastIndexOf('\n');
    if (lastNewline < 0) {
      if (atEof && text.length > 0) {
        this.remainder = '';
        this.emitRecord(continuationRecord(text));
      } else {
        this.remainder = text;
      }
      return;
    }

    const complete = text.slice(0, lastNewline + 1);
    const tail = text.slice(lastNewline + 1);
    this.remainder = tail;
    for (const line of complete.split('\n')) {
      this.emitRecordLine(line.endsWith('\r') ? line.slice(0, -1) : line);
    }
    if (atEof && tail.length > 0) {
      // Unterminated final line at EOF: emit it now rather than holding it
      // until a later poll supplies the newline.
      this.remainder = '';
      this.emitRecord(continuationRecord(tail));
    }
  }

  private emitRecordLine(line: string): void {
    if (line.length === 0) {
      return;
    }
    this.emitRecord(parseShieldLogLine(line) ?? continuationRecord(line));
  }

  private emitRecord(record: LogRecord): void {
    this.emit('record', record);
  }
}
