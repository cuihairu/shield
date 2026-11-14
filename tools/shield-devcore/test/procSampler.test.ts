import { describe, expect, it } from 'vitest';

import {
  ProcSampler,
  parseProcStat,
  parseProcStatus,
  readProcSample,
  type ProcSample,
} from '../src/sources/procSampler.js';

describe('parseProcStat', () => {
  it('reads utime/stime after a comm field containing spaces and parens', () => {
    // pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt
    // majflt cmajflt utime stime ...
    const stat =
      '1234 (node:ide server) S 1 1234 1234 0 -1 4194560 0 0 0 0 100 50 0 0 -20 0 8 0 12345 12345678 4321 18446744073709551615 1 1 1 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0';
    const cpu = parseProcStat(stat);
    expect(cpu).not.toBeNull();
    expect(cpu!.utimeTicks).toBe(100);
    expect(cpu!.stimeTicks).toBe(50);
  });

  it('reads a comm field with a closing paren inside', () => {
    // After "pid (comm)" with comm="weird)comm", the remaining fields start
    // at state (field 3): R 1 1 1 0 -1 0 0 0 0 0 0 7 3 -> utime=7 stime=3.
    const stat =
      '42 (weird)comm) R 1 1 1 0 -1 0 0 0 0 0 7 3 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0';
    const cpu = parseProcStat(stat);
    expect(cpu!.utimeTicks).toBe(7);
    expect(cpu!.stimeTicks).toBe(3);
  });

  it('returns null for garbage input', () => {
    expect(parseProcStat('no parens here')).toBeNull();
    expect(parseProcStat('1 () x y')).toBeNull();
  });
});

describe('parseProcStatus', () => {
  it('reads VmRSS and Threads', () => {
    const status = [
      'Name:\tshield',
      'VmRSS:\t  204800 kB',
      'Threads:\t12',
      'VmSize:\t 999999 kB',
    ].join('\n');
    const parsed = parseProcStatus(status);
    expect(parsed.vmRssBytes).toBe(204800 * 1024);
    expect(parsed.threads).toBe(12);
  });

  it('returns nulls for missing fields', () => {
    expect(parseProcStatus('Name:\tx')).toEqual({ vmRssBytes: null, threads: null });
  });
});

describe('readProcSample', () => {
  it('samples the live node process with sane values', async () => {
    const first = await readProcSample({ pid: process.pid, ticksPerSecond: 100 });
    expect(first.cpuSeconds).not.toBeNull();
    // First sample: 0 percent, mirroring the engine's gauge convention.
    expect(first.cpuPercent).toBe(0);
    expect(first.rssBytes).toBeGreaterThan(0);
    expect(first.threads).toBeGreaterThan(0);
    expect(first.openFds).toBeGreaterThan(0);
  });

  it('computes cpu percent as a delta over the wall interval', async () => {
    const first = await readProcSample({ pid: process.pid, ticksPerSecond: 100 });
    // Burn CPU so the delta is unambiguous.
    const spinStart = Date.now();
    let acc = 0;
    while (Date.now() - spinStart < 80) {
      acc += JSON.parse('{"x":' + Math.random() + '}').x;
    }
    void acc;
    await new Promise((r) => setTimeout(r, 30));
    const second = await readProcSample({
      pid: process.pid,
      ticksPerSecond: 100,
      previous: first,
    });
    expect(second.cpuPercent).toBeGreaterThan(0);
    expect(second.cpuSeconds!).toBeGreaterThanOrEqual(first.cpuSeconds!);
  });

  it('returns nulls for a nonexistent pid', async () => {
    const sample = await readProcSample({ pid: 4_000_000, procRoot: '/proc' });
    expect(sample.cpuSeconds).toBeNull();
    expect(sample.cpuPercent).toBeNull();
    expect(sample.rssBytes).toBeNull();
    expect(sample.threads).toBeNull();
    expect(sample.openFds).toBeNull();
  });
});

describe('ProcSampler', () => {
  it('emits samples on an interval until stopped', async () => {
    const samples: ProcSample[] = [];
    const sampler = new ProcSampler({ pid: process.pid, intervalMs: 40 });
    sampler.on('sample', (s) => samples.push(s));
    sampler.start();
    await new Promise((r) => setTimeout(r, 250));
    sampler.stop();
    const countAfterStop = samples.length;
    expect(countAfterStop).toBeGreaterThanOrEqual(2);
    await new Promise((r) => setTimeout(r, 120));
    expect(samples.length).toBe(countAfterStop);
  });
});
