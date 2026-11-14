import { afterEach, describe, expect, it } from 'vitest';
import { mkdtemp, rename, rm, truncate, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import {
  LogTail,
  continuationRecord,
  parseShieldLogLine,
  type LogRecord,
} from '../src/sources/logTail.js';

const dirs: string[] = [];
afterEach(async () => {
  while (dirs.length > 0) {
    await rm(dirs.pop()!, { recursive: true, force: true });
  }
});

async function tmpLog(name: string): Promise<string> {
  const dir = await mkdtemp(join(tmpdir(), 'shield-logtail-'));
  dirs.push(dir);
  return join(dir, name);
}

function records(tail: LogTail): LogRecord[] {
  const out: LogRecord[] = [];
  tail.on('record', (r) => out.push(r));
  return out;
}

describe('parseShieldLogLine', () => {
  it('parses the engine format_record output', () => {
    const line =
      '1762748000123 [INFO] ops: HTTP ops server listening on 127.0.0.1:8080 (src/bootstrap/bootstrap_init_servers.cpp:266)';
    const rec = parseShieldLogLine(line);
    expect(rec).not.toBeNull();
    expect(rec!.timestampMs).toBe(1762748000123);
    expect(rec!.level).toBe('info');
    expect(rec!.logger).toBe('ops');
    expect(rec!.message).toBe('HTTP ops server listening on 127.0.0.1:8080');
    expect(rec!.file).toBe('src/bootstrap/bootstrap_init_servers.cpp');
    expect(rec!.line).toBe(266);
    expect(rec!.traceId).toBeNull();
    expect(rec!.continuation).toBe(false);
  });

  it('parses the trailing trace id', () => {
    const rec = parseShieldLogLine(
      '1000 [WARNING] lua.service: dispatch slow trace=abc123',
    );
    expect(rec!.traceId).toBe('abc123');
    expect(rec!.message).toBe('dispatch slow');
    expect(rec!.level).toBe('warning');
  });

  it('parses level names to lowercase enum values', () => {
    for (const [name, level] of [
      ['DEBUG', 'debug'],
      ['INFO', 'info'],
      ['WARNING', 'warning'],
      ['ERROR', 'error'],
      ['FATAL', 'fatal'],
    ] as const) {
      expect(parseShieldLogLine(`1 [${name}] x: y`)!.level).toBe(level);
    }
  });

  it('returns null for continuation lines', () => {
    expect(parseShieldLogLine('    at some.lua:12')).toBeNull();
    expect(parseShieldLogLine('random text without header')).toBeNull();
    expect(parseShieldLogLine('')).toBeNull();
  });
});

describe('LogTail', () => {
  it('tails appended lines and parses them', async () => {
    const path = await tmpLog('shield.log');
    await writeFile(path, '1762748000001 [INFO] boot: node up\n');
    const tail = new LogTail({ path, intervalMs: 200, fromEnd: false });
    const seen = records(tail);
    tail.start();

    const waitFor = async (count: number): Promise<void> => {
      const deadline = Date.now() + 3000;
      while (seen.length < count && Date.now() < deadline) {
        await new Promise((r) => setTimeout(r, 20));
      }
    };
    await waitFor(1);
    await writeFile(path, '1762748000002 [ERROR] db: connect failed (db.cpp:42)\n', { flag: 'a' });
    await waitFor(2);
    tail.stop();

    expect(seen).toHaveLength(2);
    expect(seen[0].message).toBe('node up');
    expect(seen[1].level).toBe('error');
    expect(seen[1].file).toBe('db.cpp');
    expect(seen[1].line).toBe(42);
  });

  it('emits continuation records for non-matching lines', async () => {
    const path = await tmpLog('shield.log');
    await writeFile(
      path,
      '1762748000001 [ERROR] lua: script crashed\nstack traceback:\n\tfunction boom\n',
    );
    const tail = new LogTail({ path, intervalMs: 100, fromEnd: false });
    const seen = records(tail);
    tail.start();
    const deadline = Date.now() + 3000;
    while (seen.length < 3 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    tail.stop();
    expect(seen).toHaveLength(3);
    expect(seen[1].continuation).toBe(true);
    expect(seen[1].raw).toBe('stack traceback:');
    expect(seen[2].continuation).toBe(true);
  });

  it('handles a line split across polls', async () => {
    const path = await tmpLog('shield.log');
    const tail = new LogTail({ path, intervalMs: 100, fromEnd: false });
    const seen = records(tail);
    tail.start();

    await writeFile(path, '1762748000001 [INFO] a: partial');
    await new Promise((r) => setTimeout(r, 250));
    expect(seen).toHaveLength(0);
    await writeFile(path, ' message\n', { flag: 'a' });
    const deadline = Date.now() + 3000;
    while (seen.length < 1 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    tail.stop();
    expect(seen).toHaveLength(1);
    expect(seen[0].message).toBe('partial message');
  });

  it('detects rotation via inode change (RotatingFileSink rename + recreate)', async () => {
    const path = await tmpLog('shield.log');
    await writeFile(path, '1762748000001 [INFO] old: first\n');
    const tail = new LogTail({ path, intervalMs: 100, fromEnd: false });
    const seen = records(tail);
    let rotations = 0;
    tail.on('rotated', () => {
      rotations += 1;
    });
    tail.start();
    const deadline = Date.now() + 3000;
    while (seen.length < 1 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    const offsetBefore = seen.length;

    // RotatingFileSink semantics: rename base -> base.N, recreate base.
    // The new file is deliberately LONGER than the old offset so only the
    // inode signal can detect the swap.
    await rename(path, path + '.0');
    await writeFile(path, '1762748000002 [INFO] fresh: after rotate\n\n', { flag: 'w' });
    const deadline2 = Date.now() + 3000;
    while (seen.length < offsetBefore + 1 && Date.now() < deadline2) {
      await new Promise((r) => setTimeout(r, 20));
    }
    tail.stop();

    expect(rotations).toBe(1);
    expect(seen.some((r) => r.message === 'after rotate')).toBe(true);
  });

  it('recovers from truncation when the file shrinks below the offset', async () => {
    const path = await tmpLog('shield.log');
    await writeFile(
      path,
      '1762748000001 [INFO] old: first\n1762748000002 [INFO] old: second\n',
    );
    const tail = new LogTail({ path, intervalMs: 100, fromEnd: false });
    const seen = records(tail);
    let rotations = 0;
    tail.on('rotated', () => {
      rotations += 1;
    });
    tail.start();
    const deadline = Date.now() + 3000;
    while (seen.length < 2 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }

    // Truncation: the file shrinks below the stored offset.
    await truncate(path, 0);
    await writeFile(path, '1762748000003 [INFO] fresh: after truncate\n', { flag: 'w' });
    const deadline2 = Date.now() + 3000;
    while (!seen.some((r) => r.message === 'after truncate') && Date.now() < deadline2) {
      await new Promise((r) => setTimeout(r, 20));
    }
    tail.stop();

    expect(rotations).toBe(1);
    expect(seen.some((r) => r.message === 'after truncate')).toBe(true);
  });

  it('starts from the end of an existing file by default', async () => {
    const path = await tmpLog('shield.log');
    await writeFile(path, '1762748000001 [INFO] old: history\n');
    const tail = new LogTail({ path, intervalMs: 100 });
    const seen = records(tail);
    tail.start();
    await new Promise((r) => setTimeout(r, 250));
    await writeFile(path, '1762748000002 [INFO] new: live\n', { flag: 'a' });
    const deadline = Date.now() + 3000;
    while (seen.length < 1 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    tail.stop();
    expect(seen).toHaveLength(1);
    expect(seen[0].message).toBe('live');
  });

  it('waits quietly for a file that does not exist yet', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'shield-logtail-'));
    dirs.push(dir);
    const path = join(dir, 'later.log');
    const tail = new LogTail({ path, intervalMs: 50, fromEnd: false });
    const seen = records(tail);
    const errors: Error[] = [];
    tail.on('error', (e) => errors.push(e));
    tail.start();
    await new Promise((r) => setTimeout(r, 200));
    await writeFile(path, '1762748000001 [INFO] boot: created\n');
    const deadline = Date.now() + 3000;
    while (seen.length < 1 && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 20));
    }
    tail.stop();
    expect(errors).toHaveLength(0);
    expect(seen).toHaveLength(1);
  });

  it('continuationRecord marks raw-only payloads', () => {
    const rec = continuationRecord('  extra detail');
    expect(rec.continuation).toBe(true);
    expect(rec.raw).toBe('  extra detail');
    expect(rec.timestampMs).toBeNull();
  });
});
