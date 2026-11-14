// [SHIELD_DEVCORE] Client for the engine's ops HTTP surface
// (src/console/ops_http_handler.cpp): /ops/health, /ops/metrics (Prometheus
// text), /ops/services and /ops/services/:name. Every endpoint except
// /ops/health sits behind the ops-wide bearer gate (http.ops_auth_token,
// default-deny), so the token is optional but normally required.

export interface OpsHttpOptions {
  /// Base URL of the ops server, e.g. http://127.0.0.1:8080
  baseUrl: string;
  /// Bearer token for the ops-wide gate (http.ops_auth_token). Omit only
  /// when the node sets http.ops_allow_unauthenticated=true.
  token?: string;
  /// Bearer token for /ops/eval and /ops/profile (http.profile_token and
  /// http.eval_token live behind their own gates, which must match this
  /// single Authorization header; unset means those endpoints cannot work).
  profileToken?: string;
  /// Per-request timeout in ms (default 4000; the engine answers most
  /// endpoints within a 2s actor-mesh wait).
  timeoutMs?: number;
}

export interface OpsHealth {
  ok: boolean;
  data: Record<string, unknown>;
}

export interface PromSample {
  /// Sample name: family name or family{labels} rendered name.
  name: string;
  family: string;
  labels: Record<string, string>;
  value: number;
}

export interface PromFamily {
  name: string;
  type: string;
  help: string;
  samples: PromSample[];
}

export interface PromSnapshot {
  /// family name -> family
  families: Map<string, PromFamily>;
  /// flat sample lookup by rendered "family{label=value,...}" name
  byName: Map<string, PromSample>;
  text: string;
}

export interface ProfileRequest {
  action: 'start' | 'stop' | 'report' | 'status';
  /// Required for action=start.
  service?: string;
  /// 1..60000 (engine validation).
  duration_ms?: number;
  /// >=1 samples per second granularity knob.
  interval?: number;
}

/// One node of the engine's sampling call tree (export_node in
/// src/lua/profile_session.cpp): pct is the share of total samples.
export interface ProfileReportNode {
  what: string;
  name: string;
  source: string;
  line: number;
  tail: boolean;
  hits: number;
  pct: number;
  children: ProfileReportNode[];
}

export interface ProfileResponse {
  active?: boolean;
  service?: string;
  duration_ms?: number;
  interval?: number;
  started?: boolean;
  started_at?: number;
  stopped_at?: number;
  elapsed_ms?: number;
  /// Present in stop/report responses (finish_report in
  /// src/lua/profile_session.cpp): total hook samples and the call-tree
  /// roots (each with recursive children), sorted by hits.
  total_samples?: number;
  truncated_frames?: number;
  dropped_samples?: number;
  frames?: ProfileReportNode[];
  slow_calls?: unknown;
  [key: string]: unknown;
}

export interface LuaServiceDetail {
  name: string;
  state: string;
  /// Script path as recorded by the engine (config-relative or absolute;
  /// resolve against workspace roots before opening in the editor).
  script?: string;
  rpcRoutes?: number;
  requests?: number;
  errors?: number;
  uptimeSeconds?: number;
  timers?: number;
  pendingCalls?: number;
  pendingTasks?: number;
  coroutines?: number;
  memoryKb?: number;
}

export class OpsHttpError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
  }
}

/// Parse Prometheus 0.0.4 text into families and samples. Pure function;
/// tolerant of blank lines and comment lines without HELP/TYPE.
export function parsePrometheusText(text: string): PromSnapshot {
  const families = new Map<string, PromFamily>();
  const byName = new Map<string, PromSample>();
  let pendingHelp = '';
  let pendingType = '';

  const lines = text.split('\n');
  for (const rawLine of lines) {
    const line = rawLine.trim();
    if (line.length === 0) {
      continue;
    }
    if (line.startsWith('#')) {
      if (line.startsWith('# HELP ')) {
        const rest = line.slice('# HELP '.length);
        const spaceIdx = rest.indexOf(' ');
        pendingHelp = spaceIdx >= 0 ? rest.slice(spaceIdx + 1) : rest;
        pendingType = '';
      } else if (line.startsWith('# TYPE ')) {
        const rest = line.slice('# TYPE '.length);
        const spaceIdx = rest.indexOf(' ');
        pendingType = spaceIdx >= 0 ? rest.slice(spaceIdx + 1) : 'untyped';
        pendingHelp = '';
      }
      continue;
    }

    const spaceIdx = findValueSeparator(line);
    if (spaceIdx < 0) {
      continue;
    }
    const metricPart = line.slice(0, spaceIdx).trim();
    const valuePart = line.slice(spaceIdx + 1).trim();
    const value = Number(valuePart);
    if (!Number.isFinite(value) && valuePart !== 'NaN' && valuePart !== '+Inf' && valuePart !== '-Inf') {
      continue;
    }

    let family = metricPart;
    const labels: Record<string, string> = {};
    const braceStart = metricPart.indexOf('{');
    if (braceStart >= 0 && metricPart.endsWith('}')) {
      family = metricPart.slice(0, braceStart);
      const labelBlob = metricPart.slice(braceStart + 1, -1);
      for (const pair of splitLabels(labelBlob)) {
        const eq = pair.indexOf('=');
        if (eq < 0) {
          continue;
        }
        const key = pair.slice(0, eq).trim();
        let valueStr = pair.slice(eq + 1).trim();
        if (valueStr.startsWith('"') && valueStr.endsWith('"')) {
          valueStr = unescapePromString(valueStr.slice(1, -1));
        }
        labels[key] = valueStr;
      }
    }

    const fam =
      families.get(family) ??
      ({ name: family, type: 'untyped', help: '', samples: [] } as PromFamily);
    if (fam.type === 'untyped' && pendingType) {
      fam.type = pendingType;
    }
    if (!fam.help && pendingHelp) {
      fam.help = pendingHelp;
    }
    const sample: PromSample = { name: metricPart, family, labels, value };
    fam.samples.push(sample);
    families.set(family, fam);
    byName.set(metricPart, sample);
    pendingHelp = '';
    pendingType = '';
  }

  return { families, byName, text };
}

/// The value is separated from the metric part by whitespace, but only
/// outside of braces (a label value could contain a space).
function findValueSeparator(line: string): number {
  let inBraces = false;
  let inString = false;
  for (let i = 0; i < line.length; i++) {
    const ch = line[i];
    if (inString) {
      if (ch === '\\') {
        i++;
      } else if (ch === '"') {
        inString = false;
      }
      continue;
    }
    if (ch === '"') {
      inString = true;
    } else if (ch === '{') {
      inBraces = true;
    } else if (ch === '}') {
      inBraces = false;
    } else if (ch === ' ' && !inBraces) {
      return i;
    }
  }
  return -1;
}

function* splitLabels(blob: string): Generator<string> {
  let inString = false;
  let start = 0;
  for (let i = 0; i < blob.length; i++) {
    const ch = blob[i];
    if (inString) {
      if (ch === '\\') {
        i++;
      } else if (ch === '"') {
        inString = false;
      }
      continue;
    }
    if (ch === '"') {
      inString = true;
    } else if (ch === ',') {
      const part = blob.slice(start, i).trim();
      if (part) {
        yield part;
      }
      start = i + 1;
    }
  }
  const last = blob.slice(start).trim();
  if (last) {
    yield last;
  }
}

function unescapePromString(s: string): string {
  return s.replace(/\\n/g, '\n').replace(/\\\\/g, '\\').replace(/\\"/g, '"');
}

interface Envelope<T> {
  type: string;
  data: T;
}

export class OpsHttpClient {
  private readonly baseUrl: string;
  private readonly token: string;
  private readonly profileToken: string;
  private readonly timeoutMs: number;
  private readonly headers: Record<string, string>;

  constructor(options: OpsHttpOptions) {
    this.baseUrl = options.baseUrl.replace(/\/+$/, '');
    this.token = options.token ?? '';
    this.profileToken = options.profileToken ?? '';
    this.timeoutMs = options.timeoutMs ?? 4000;
    this.headers = this.token ? { authorization: `Bearer ${this.token}` } : {};
  }

  /// Headers for the profile/eval gates: the endpoint's own token is the
  /// authorization there (a single header cannot carry both tokens).
  private profileHeaders(): Record<string, string> {
    return this.profileToken
      ? { authorization: `Bearer ${this.profileToken}`, 'content-type': 'application/json' }
      : { 'content-type': 'application/json' };
  }

  get base(): string {
    return this.baseUrl;
  }

  private async getJson(path: string): Promise<Envelope<unknown>> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const res = await fetch(`${this.baseUrl}${path}`, {
        headers: this.headers,
        signal: controller.signal,
      });
      if (!res.ok) {
        throw new OpsHttpError(`GET ${path} -> ${res.status}`, res.status);
      }
      return (await res.json()) as Envelope<unknown>;
    } finally {
      clearTimeout(timer);
    }
  }

  private async postJson(
    path: string,
    body: unknown,
    headers?: Record<string, string>,
  ): Promise<Envelope<unknown>> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const res = await fetch(`${this.baseUrl}${path}`, {
        method: 'POST',
        headers: headers ?? { ...this.headers, 'content-type': 'application/json' },
        body: JSON.stringify(body),
        signal: controller.signal,
      });
      if (!res.ok) {
        throw new OpsHttpError(`POST ${path} -> ${res.status}`, res.status);
      }
      return (await res.json()) as Envelope<unknown>;
    } finally {
      clearTimeout(timer);
    }
  }

  private async getText(path: string): Promise<string> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const res = await fetch(`${this.baseUrl}${path}`, {
        headers: this.headers,
        signal: controller.signal,
      });
      if (!res.ok) {
        throw new OpsHttpError(`GET ${path} -> ${res.status}`, res.status);
      }
      return await res.text();
    } finally {
      clearTimeout(timer);
    }
  }

  /// GET /ops/health — unauthenticated by design (k8s probes). The engine
  /// answers { data: { checks: {...}, status: "ok"|"degraded", uptime } };
  /// prefer the top-level status and fall back to the per-check scan.
  async health(): Promise<OpsHealth> {
    const envelope = await this.getJson('/ops/health');
    const data = (envelope.data ?? {}) as Record<string, unknown>;
    if (typeof data.status === 'string') {
      return { ok: data.status === 'ok', data };
    }
    const ok = Object.values(data.checks ?? {}).every(
      (check) =>
        typeof check !== 'object' ||
        check === null ||
        (check as Record<string, unknown>).status !== 'degraded',
    );
    return { ok, data };
  }

  /// GET /ops/metrics — Prometheus text export.
  async metrics(): Promise<PromSnapshot> {
    return parsePrometheusText(await this.getText('/ops/metrics'));
  }

  /// GET /ops/services — published service names ([] on actor-mesh timeout,
  /// which the engine reports as the string "timeout" inside a 200 envelope).
  async services(): Promise<string[]> {
    const envelope = await this.getJson('/ops/services');
    return Array.isArray(envelope.data) ? (envelope.data as string[]) : [];
  }

  /// GET /ops/services/:name — per-service snapshot; null when 404.
  async serviceDetail(name: string): Promise<LuaServiceDetail | null> {
    let envelope: Envelope<unknown>;
    try {
      envelope = await this.getJson(
        `/ops/services/${encodeURIComponent(name)}`,
      );
    } catch (err) {
      if (err instanceof OpsHttpError && err.status === 404) {
        return null;
      }
      throw err;
    }
    const data = envelope.data as Record<string, unknown> | null;
    if (!data) {
      return null;
    }
    return {
      name: String(data.name ?? name),
      state: String(data.state ?? 'unknown'),
      script: typeof data.script === 'string' ? data.script : undefined,
      rpcRoutes: typeof data.rpc_routes === 'number' ? data.rpc_routes : undefined,
      requests: typeof data.requests === 'number' ? data.requests : undefined,
      errors: typeof data.errors === 'number' ? data.errors : undefined,
      uptimeSeconds:
        typeof data.uptime_seconds === 'number' ? data.uptime_seconds : undefined,
      timers: typeof data.timers === 'number' ? data.timers : undefined,
      pendingCalls:
        typeof data.pending_calls === 'number' ? data.pending_calls : undefined,
      pendingTasks:
        typeof data.pending_tasks === 'number' ? data.pending_tasks : undefined,
      coroutines:
        typeof data.coroutines === 'number' ? data.coroutines : undefined,
      memoryKb: typeof data.memory_kb === 'number' ? data.memory_kb : undefined,
    };
  }

  /// Convenience: every published service with its detail snapshot.
  async allServiceDetails(): Promise<LuaServiceDetail[]> {
    const names = await this.services();
    const details = await Promise.all(names.map((n) => this.serviceDetail(n)));
    return details.filter((d): d is LuaServiceDetail => d !== null);
  }

  /// POST /ops/profile — the sampling profiler control surface (opt-in,
  /// gated by http.profile_token, a separate token from the ops-wide gate).
  /// actions: start (needs service), stop, report, status.
  async profile(
    body: ProfileRequest,
  ): Promise<ProfileResponse> {
    const envelope = await this.postJson('/ops/profile', body, this.profileHeaders());
    return envelope.data as ProfileResponse;
  }

  /// One-shot sampling: start a session for `durationMs` on `service`,
  /// wait it out, then fetch the report.
  async sampleOnce(
    service: string,
    durationMs = 3000,
  ): Promise<ProfileResponse> {
    await this.profile({ action: 'start', service, duration_ms: durationMs });
    await new Promise((resolve) => setTimeout(resolve, durationMs + 300));
    return await this.profile({ action: 'report' });
  }
}

/// Process-level gauges the engine exposes on /ops/metrics (Linux; omitted
/// by the engine on platforms without /proc). Pull them out of a snapshot.
export function processGauges(snapshot: PromSnapshot): {
  cpuPercent: number | null;
  rssBytes: number | null;
  threads: number | null;
  openFds: number | null;
  uptimeSeconds: number | null;
  serviceCount: number | null;
} {
  const gauge = (family: string): number | null => {
    const s = snapshot.byName.get(family);
    return s && Number.isFinite(s.value) ? s.value : null;
  };
  return {
    cpuPercent: gauge('shield_process_cpu_percent'),
    rssBytes: gauge('shield_process_rss_bytes'),
    threads: gauge('shield_process_threads'),
    openFds: gauge('shield_process_open_fds'),
    uptimeSeconds: gauge('shield_uptime_seconds'),
    serviceCount: gauge('shield_services'),
  };
}
