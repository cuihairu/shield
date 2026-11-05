import { useEffect, useState } from 'react';
import { Alert, Button, Empty, Input, Space, Spin, Table, Tag } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { api } from '../services/api';
import LoginGate from '../components/LoginGate';

// The /ops/config endpoint is single-key (never a full snapshot), so the page
// queries an explicit set of ops-relevant keys. Redaction is decided by the
// server: when it returns the sentinel below the key is shown as redacted and
// the original value is never echoed. The client never guesses a secret from
// the key name.
const REDACTED = '***redacted***';

const OPS_KEYS = [
  'http.host',
  'http.port',
  'http.ops_auth_token',
  'http.static_root',
  'http.eval_enabled',
  'http.eval_token',
  'http.profile_enabled',
  'http.profile_token',
  'http.slow_call_threshold_ms',
  'http.profile_cooldown_seconds',
];

interface ConfigRow {
  key: string;
  value: string | null;
  redacted: boolean;
}

export default function ConfigPage() {
  const [rows, setRows] = useState<ConfigRow[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [pending, setPending] = useState(false);
  const [queryKey, setQueryKey] = useState('');
  const [unauthorized, setUnauthorized] = useState(false);

  useEffect(() => {
    const controller = new AbortController();
    const entries = new Map<string, ConfigRow>();
    OPS_KEYS.forEach((k) => entries.set(k, { key: k, value: null, redacted: false }));
    let settled = 0;
    OPS_KEYS.forEach((k) => {
      api
        .config(k, controller.signal)
        .then((value) => {
          entries.set(k, { key: k, value, redacted: value === REDACTED });
        })
        .catch((e) => {
          const msg = e instanceof Error ? e.message : String(e);
          if (msg === 'Unauthorized') {
            setUnauthorized(true);
          } else {
            setError(msg);
          }
        })
        .finally(() => {
          settled += 1;
          if (settled === OPS_KEYS.length) {
            setRows(Array.from(entries.values()));
            setLoading(false);
          }
        });
    });
    return () => controller.abort();
  }, []);

  async function addKey() {
    const key = queryKey.trim();
    if (!key) return;
    if (pending) return; // gate: an in-flight query owns the input
    setPending(true);
    try {
      const value = await api.config(key);
      setRows((prev) => {
        const rest = (prev ?? []).filter((r) => r.key !== key);
        return [...rest, { key, value, redacted: value === REDACTED }];
      });
      setQueryKey('');
      setError(null);
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      if (msg === 'Unauthorized') {
        setUnauthorized(true);
      } else {
        setError(msg);
      }
    } finally {
      setPending(false);
    }
  }

  if (loading) {
    return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  }
  if (unauthorized) {
    return <LoginGate />;
  }
  if (error && !rows) {
    return <Alert type="error" showIcon message="Failed to load config" description={error} />;
  }

  const columns: ColumnsType<ConfigRow> = [
    { title: 'Key', dataIndex: 'key' },
    {
      title: 'Value',
      dataIndex: 'value',
      render: (v: string | null, row) => {
        if (row.redacted) return <Tag color="red">已脱敏</Tag>;
        if (v === null) return <Tag>未配置</Tag>;
        return <code>{v}</code>;
      },
    },
    {
      title: 'Status',
      key: 'status',
      render: (_, row) => {
        if (row.redacted) return <Tag color="red">redacted (server)</Tag>;
        if (row.value === null) return <Tag>unset</Tag>;
        return <Tag color="green">plaintext</Tag>;
      },
    },
  ];

  return (
    <div>
      {error && (
        <Alert type="warning" showIcon style={{ marginBottom: 16 }}
          message="Config query failed" description={error} />
      )}
      <Space style={{ marginBottom: 16 }}>
        <Input
          placeholder="Query a config key, e.g. database.host"
          value={queryKey}
          onChange={(e) => setQueryKey(e.target.value)}
          onPressEnter={addKey}
          style={{ width: 320 }}
          allowClear
        />
        <Button type="primary" onClick={addKey} loading={pending}>
          Query
        </Button>
      </Space>
      {!rows || rows.length === 0 ? (
        <Empty description="No config keys" />
      ) : (
        <Table<ConfigRow>
          rowKey="key"
          columns={columns}
          dataSource={rows}
          pagination={false}
          size="small"
          title={() => 'Configuration (redacted keys never echo the original value)'}
        />
      )}
    </div>
  );
}
