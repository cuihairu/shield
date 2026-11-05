import { useRef } from 'react';
import { Alert, Empty, Spin, Table, Tag } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { usePolling } from '../services/polling';
import { api } from '../services/api';

interface PortRow {
  port: string;
  connections: number;
  active: number;
  rejections: number;
  rateLimited: number;
  tlsOk: number;
  tlsFail: number;
}

function parseSample(line: string): { name: string; labels: Record<string, string>; value: number } | null {
  const m = /^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{([^}]*)\})?\s+([-0-9.eE+]+)$/.exec(line.trim());
  if (!m) return null;
  const labels: Record<string, string> = {};
  if (m[3]) {
    for (const kv of m[3].split(',')) {
      const eq = kv.indexOf('=');
      if (eq > 0) labels[kv.slice(0, eq)] = kv.slice(eq + 1).replace(/"/g, '');
    }
  }
  return { name: m[1], labels, value: parseFloat(m[4]) };
}

function buildRows(text: string): PortRow[] {
  const byPort = new Map<string, PortRow>();
  for (const line of text.split('\n')) {
    const s = parseSample(line);
    if (!s || !s.labels.port) continue;
    const port = s.labels.port;
    let row = byPort.get(port);
    if (!row) {
      row = { port, connections: 0, active: 0, rejections: 0, rateLimited: 0, tlsOk: 0, tlsFail: 0 };
      byPort.set(port, row);
    }
    switch (s.name) {
      case 'shield_gateway_connections_total': row.connections = s.value; break;
      case 'shield_gateway_active_sessions': row.active = s.value; break;
      case 'shield_gateway_rejections_total': row.rejections += s.value; break;
      case 'shield_gateway_rate_limited_messages_total': row.rateLimited = s.value; break;
      case 'shield_gateway_tls_handshakes_total': row.tlsOk = s.value; break;
      case 'shield_gateway_tls_handshake_failures_total': row.tlsFail = s.value; break;
    }
  }
  return Array.from(byPort.values()).sort((a, b) => a.port.localeCompare(b.port));
}

// Counters that must never go backwards between snapshots: a regression is a
// real anomaly (listener restart resets counters), not a transient scrape
// artifact. Gauges are excluded — active sessions legitimately rise and fall.
const COUNTER_KEYS = ['connections', 'rejections', 'rateLimited', 'tlsOk', 'tlsFail'] as const;

export default function GatewayPage() {
  const prevCounters = useRef<Map<string, number>>(new Map());
  const poll = usePolling<string>(
    (signal) => api.metricsText(signal),
    { intervalMs: 2000, pauseWhenHidden: true, hasData: (t) => t.length > 0 },
  );

  if (poll.status === 'loading') {
    return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  }
  if (poll.status === 'unauthorized') {
    return <Alert type="warning" showIcon message="Unauthorized"
      description="Authentication is required to view this console." />;
  }
  if (poll.status === 'error') {
    return <Alert type="error" showIcon message="Failed to load gateway metrics" description={poll.error} />;
  }
  if (!poll.data || poll.data.length === 0) {
    return <Empty description="No listeners" />;
  }

  const rows = buildRows(poll.data);
  if (rows.length === 0) {
    return <Empty description="No listeners" />;
  }

  // Counter monotonicity across snapshots. A counter that went backwards is a
  // real anomaly (listener restart resets counters), surfaced as a warning.
  let regressed = false;
  const currentCounters = new Map<string, number>();
  for (const r of rows) {
    for (const key of COUNTER_KEYS) {
      const counterKey = `${r.port}:${key}`;
      currentCounters.set(counterKey, r[key]);
      const prev = prevCounters.current.get(counterKey);
      if (prev !== undefined && r[key] < prev) regressed = true;
    }
  }
  prevCounters.current = currentCounters;

  const cols: ColumnsType<PortRow> = [
    { title: 'Port', dataIndex: 'port' },
    { title: 'Connections (counter)', dataIndex: 'connections' },
    { title: 'Active sessions (gauge)', dataIndex: 'active', render: (v: number) => <Tag color="blue">{v}</Tag> },
    { title: 'Rejections (counter)', dataIndex: 'rejections' },
    { title: 'Rate limited (counter)', dataIndex: 'rateLimited' },
    { title: 'TLS ok (counter)', dataIndex: 'tlsOk' },
    { title: 'TLS fail (counter)', dataIndex: 'tlsFail' },
  ];

  return (
    <div>
      {poll.stale && (
        <Alert type="warning" showIcon style={{ marginBottom: 16 }}
          message="Data is stale"
          description="The ops server is unreachable; showing last successful snapshot. New data will appear when the connection recovers." />
      )}
      {regressed && (
        <Alert type="warning" showIcon style={{ marginBottom: 16 }}
          message="Counter regression detected"
          description="A gateway counter went backwards; this may indicate a listener restart." />
      )}
      <Table<PortRow> rowKey="port" columns={cols} dataSource={rows} pagination={false} size="small"
        title={() => 'Gateway connections & rate limits (per listener)'} />
    </div>
  );
}
