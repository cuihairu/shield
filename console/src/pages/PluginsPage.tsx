import { useEffect, useState } from 'react';
import { Alert, Empty, Spin, Table, Tag } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { api } from '../services/api';
import type { PluginInstance, PoolMetric } from '../types';

// Parse a Prometheus exposition sample line into {name, labels, value}.
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
  const [plugins, setPlugins] = useState<PluginInstance[] | null>(null);
  const [pools, setPools] = useState<PoolMetric[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    const controller = new AbortController();
    Promise.all([api.plugins(controller.signal), api.metricsText(controller.signal)])
      .then(([pl, metrics]) => {
        setPlugins(pl);
        setPools(extractPoolMetrics(metrics));
        setError(null);
      })
      .catch((e) => {
        setError(e instanceof Error ? e.message : String(e));
      })
      .finally(() => setLoading(false));
    return () => controller.abort();
  }, []);

  if (loading) return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  if (error) return <Alert type="error" showIcon message="Failed to load plugins" description={error} />;
  if (!plugins || plugins.length === 0) return <Empty description="No plugins" />;

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
