import { Alert, Empty, Spin, Table, Tag } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { api } from '../services/api';
import type { PluginInstance, PoolMetric } from '../types';
import { usePolling } from '../services/polling';
import LoginGate from '../components/LoginGate';

// Parse a Prometheus exposition sample line into {name, labels, value}.
function parseSample(line: string): { name: string; labels: Record<string, string>; value: number } | null {
  const m = /^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{([^}]*)\})?\s+([-+]?(?:Inf|NaN|[0-9]+(?:\.[0-9]*)?(?:[eE][-+]?[0-9]+)?))$/.exec(line.trim());
  if (!m) return null;
  const labels: Record<string, string> = {};
  if (m[3]) {
    const parts: string[] = [];
    let depth = 0;
    let cur = '';
    for (const ch of m[3]) {
      if (ch === '"') depth ^= 1;
      if (ch === ',' && depth === 0) {
        parts.push(cur);
        cur = '';
      } else {
        cur += ch;
      }
    }
    if (cur) parts.push(cur);
    for (const kv of parts) {
      const eq = kv.indexOf('=');
      if (eq > 0) {
        const raw = kv.slice(eq + 1).trim();
        const value = raw.startsWith('"') && raw.endsWith('"')
          ? raw.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, '\\')
          : raw;
        labels[kv.slice(0, eq).trim()] = value;
      }
    }
  }
  const raw = m[4];
  const value = raw === '+Inf' ? Infinity : raw === '-Inf' ? -Infinity : raw === 'NaN' ? NaN : parseFloat(raw);
  return { name: m[1], labels, value };
}

function extractPoolMetrics(text: string): PoolMetric[] {
  const byKey = new Map<string, PoolMetric>();
  for (const line of text.split('\n')) {
    const sample = parseSample(line);
    if (!sample) continue;
    const family = sample.name;
    if (!family.startsWith('shield_plugin_pool_') && family !== 'shield_plugin_db_pending_async' && family !== 'shield_plugin_db_holding') continue;
    const plugin = sample.labels.plugin ?? '';
    const instance = sample.labels.instance ?? '';
    const key = `${plugin}|${instance}`;
    let entry = byKey.get(key);
    if (!entry) {
      entry = { plugin, instance };
      byKey.set(key, entry);
    }
    if (family === 'shield_plugin_pool_size') entry.pool_size = sample.value;
    else if (family === 'shield_plugin_pool_in_use') entry.in_use = sample.value;
    else if (family === 'shield_plugin_pool_idle') entry.idle = sample.value;
    else if (family === 'shield_plugin_db_pending_async') entry.pending_async = sample.value;
    else if (family === 'shield_plugin_db_holding') entry.holding = sample.value;
  }
  return Array.from(byKey.values());
}

export default function PluginsPage() {
  // Poll like the other pages so a dropped connection surfaces as a stale
  // banner instead of a silent one-shot page frozen at mount.
  const poll = usePolling<{ plugins: PluginInstance[]; pools: PoolMetric[] }>(
    (signal) =>
      Promise.all([api.plugins(signal), api.metricsText(signal)]).then(
        ([pl, metrics]) => ({ plugins: pl, pools: extractPoolMetrics(metrics) }),
      ),
    { intervalMs: 2000, pauseWhenHidden: true, hasData: (d) => d.plugins.length > 0 },
  );

  if (poll.status === 'loading') return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  if (poll.status === 'unauthorized') return <LoginGate />;
  if (poll.status === 'error') return <Alert type="error" showIcon message="Failed to load plugins" description={poll.error} />;
  if (!poll.data || poll.data.plugins.length === 0) return <Empty description="No plugins" />;

  const plugins = poll.data.plugins;
  const pools = poll.data.pools;

  const requiredDegraded = plugins.some((p) => p.required && p.state !== 'started');

  const pluginCols: ColumnsType<PluginInstance> = [
    { title: 'ID', dataIndex: 'id' },
    { title: 'Package', dataIndex: 'package' },
    {
      title: 'State',
      dataIndex: 'state',
      render: (s: string, row) => {
        const bad = row.required && s !== 'started';
        return <Tag color={bad ? 'red' : s === 'started' ? 'green' : 'default'}>{s}</Tag>;
      },
    },
    { title: 'Required', dataIndex: 'required', render: (r: boolean) => (r ? 'yes' : 'no') },
  ];

  const poolCols: ColumnsType<PoolMetric> = [
    { title: 'Plugin', dataIndex: 'plugin' },
    { title: 'Instance', dataIndex: 'instance' },
    { title: 'Pool size', dataIndex: 'pool_size' },
    { title: 'In use', dataIndex: 'in_use' },
    { title: 'Idle', dataIndex: 'idle' },
    { title: 'DB pending async', dataIndex: 'pending_async' },
    { title: 'DB holding', dataIndex: 'holding' },
  ];

  return (
    <div>
      {poll.stale && (
        <Alert type="warning" showIcon style={{ marginBottom: 16 }}
          message="Data is stale"
          description="The ops server is unreachable; showing last successful snapshot. New data will appear when the connection recovers." />
      )}
      {requiredDegraded && (
        <Alert type="error" showIcon style={{ marginBottom: 16 }}
          message="Required plugin degraded" description="At least one required plugin is not started; health reports degraded accordingly." />
      )}
      <Table<PluginInstance> rowKey="id" columns={pluginCols} dataSource={plugins}
        pagination={false} size="small" title={() => 'Plugin instances'} />
      <Table<PoolMetric> rowKey={(r) => `${r.plugin}|${r.instance}`} columns={poolCols}
        dataSource={pools} pagination={false} size="small" style={{ marginTop: 16 }}
        title={() => 'Connection pools'} />
    </div>
  );
}
