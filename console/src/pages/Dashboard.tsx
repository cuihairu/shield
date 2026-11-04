import { useEffect, useState } from 'react';
import { Alert, Badge, Card, Col, Empty, Row, Spin, Statistic, Table, Tag } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { api } from '../services/api';
import type { ClusterNode, HealthData, OpsStatusData } from '../types';

type Status = 'loading' | 'ready' | 'empty' | 'error' | 'unauthorized';

function formatUptime(sec: number): string {
  if (sec < 60) return `${sec.toFixed(0)}s`;
  const m = Math.floor(sec / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h}h ${m % 60}m`;
  const d = Math.floor(h / 24);
  return `${d}d ${h % 24}h`;
}

const nodeColumns: ColumnsType<ClusterNode> = [
  { title: 'Node', dataIndex: 'node_id', key: 'node_id' },
  { title: 'Address', dataIndex: 'address', key: 'address' },
  {
    title: 'State',
    dataIndex: 'state',
    key: 'state',
    render: (s: string) => <Tag color={s === 'online' ? 'green' : 'red'}>{s}</Tag>,
  },
  { title: 'Epoch', dataIndex: 'epoch', key: 'epoch' },
  {
    title: 'Heartbeat age',
    dataIndex: 'heartbeat_age_ms',
    key: 'heartbeat_age_ms',
    render: (ms: number | undefined) => (ms == null ? '-' : `${(ms / 1000).toFixed(1)}s`),
  },
];

export default function Dashboard() {
  const [status, setStatus] = useState<Status>('loading');
  const [health, setHealth] = useState<HealthData | null>(null);
  const [opsStatus, setOpsStatus] = useState<OpsStatusData | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [stale, setStale] = useState(false);
  const [lastOk, setLastOk] = useState<number | null>(null);

  useEffect(() => {
    const controller = new AbortController();
    let alive = true;
    let timer: ReturnType<typeof setInterval> | null = null;

    async function poll() {
      try {
        const [h, s] = await Promise.all([
          api.health(controller.signal),
          api.status(controller.signal),
        ]);
        if (!alive) return;
        setHealth(h.data);
        setOpsStatus(s.data);
        setStale(false);
        setLastOk(Date.now());
        const hasData = Object.keys(h.data?.checks ?? {}).length > 0 || !!s.data?.server;
        setStatus(hasData ? 'ready' : 'empty');
        setError(null);
      } catch (e) {
        if (!alive || controller.signal.aborted) return;
        const msg = e instanceof Error ? e.message : String(e);
        if (msg.includes('401') || msg.includes('Unauthorized')) {
          setStatus('unauthorized');
        } else {
          // Network failure: keep last data but mark it stale (never show
          // a frozen value as current).
          setStale(true);
          if (lastOk === null) {
            setStatus('error');
            setError(msg);
          } else {
            setStatus('ready');
            setError(null);
          }
        }
      }
    }

    poll();
    timer = setInterval(poll, 2000);
    return () => {
      alive = false;
      controller.abort();
      if (timer) clearInterval(timer);
    };
  }, [lastOk]);

  if (status === 'loading') {
    return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  }
  if (status === 'unauthorized') {
    return <Alert type="warning" showIcon message="Unauthorized" description="Authentication is required to view this console." />;
  }
  if (status === 'error') {
    return <Alert type="error" showIcon message="Failed to load data" description={error} />;
  }
  if (status === 'empty') {
    return <Empty description="No data available" style={{ margin: '80px auto' }} />;
  }

  const degraded = health?.status === 'degraded';
  const checkCards = health
    ? Object.entries(health.checks).map(([name, check]) => (
        <Col span={8} key={name}>
          <Card size="small" title={name}>
            <Badge status={check.status === 'ok' ? 'success' : 'error'} text={check.status} />
            {check.uptime_seconds != null && (
              <div style={{ marginTop: 8 }}>Uptime: {formatUptime(check.uptime_seconds)}</div>
            )}
            {check.started != null && (
              <div style={{ marginTop: 8 }}>Started: {check.started}</div>
            )}
            {check.required_down != null && (
              <div style={{ marginTop: 8 }}>Required down: {check.required_down}</div>
            )}
          </Card>
        </Col>
      ))
    : [];

  return (
    <div>
      {stale && (
        <Alert type="warning" showIcon style={{ marginBottom: 16 }}
          message="Data is stale" description="The ops server is unreachable; showing last successful snapshot. New data will appear when the connection recovers." />
      )}
      {degraded && (
        <Alert type="error" showIcon style={{ marginBottom: 16 }}
          message="Degraded" description="Health status is degraded (HTTP 503)." />
      )}
      <Row gutter={16}>
        <Col span={8}>
          <Card size="small">
            <Statistic title="Overall" value={health?.status ?? 'unknown'} valueStyle={{ color: degraded ? '#cf1322' : '#3f8600' }} />
          </Card>
        </Col>
        <Col span={8}>
          <Card size="small">
            <Statistic title="Uptime (s)" value={health?.uptime ?? 0} />
          </Card>
        </Col>
        <Col span={8}>
          <Card size="small">
            <Statistic title="Server state" value={opsStatus?.server?.state ?? 'n/a'} />
          </Card>
        </Col>
      </Row>
      <Row gutter={16} style={{ marginTop: 16 }}>{checkCards}</Row>
      {opsStatus?.cluster && (
        <Card title="Cluster nodes" size="small" style={{ marginTop: 16 }}>
          <Table<ClusterNode> rowKey="node_id" columns={nodeColumns}
            dataSource={opsStatus.cluster.nodes} pagination={false} size="small" />
        </Card>
      )}
      {opsStatus?.server && (
        <Card title="Server state machine" size="small" style={{ marginTop: 16 }}>
          <Row gutter={16}>
            <Col span={8}><Statistic title="State" value={opsStatus.server.state} /></Col>
            <Col span={8}><Statistic title="Uptime" value={formatUptime(opsStatus.server.uptime_seconds ?? 0)} /></Col>
            <Col span={8}><Statistic title="Watchers" value={opsStatus.server.watchers ?? 0} /></Col>
          </Row>
          {opsStatus.server.shutdown_scheduled && (
            <div style={{ marginTop: 8 }}><Tag color="orange">Shutdown scheduled</Tag></div>
          )}
        </Card>
      )}
    </div>
  );
}
