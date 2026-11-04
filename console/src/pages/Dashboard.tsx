import { useEffect, useState } from 'react';
import { Alert, Card, Col, Empty, Row, Spin, Statistic } from 'antd';
import { api } from '../services/api';
import type { HealthData, OpsStatusData } from '../types';

type Status = 'loading' | 'ready' | 'empty' | 'error' | 'unauthorized';

export default function Dashboard() {
  const [status, setStatus] = useState<Status>('loading');
  const [health, setHealth] = useState<HealthData | null>(null);
  const [opsStatus, setOpsStatus] = useState<OpsStatusData | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const controller = new AbortController();
    let alive = true;

    async function poll() {
      try {
        const [h, s] = await Promise.all([
          api.health(controller.signal),
          api.status(controller.signal),
        ]);
        if (!alive) return;
        setHealth(h);
        setOpsStatus(s);
        const hasData = Object.keys(h?.checks ?? {}).length > 0 || !!s?.server;
        setStatus(hasData ? 'ready' : 'empty');
        setError(null);
      } catch (e) {
        if (!alive || controller.signal.aborted) return;
        const msg = e instanceof Error ? e.message : String(e);
        if (msg.includes('401') || msg.includes('Unauthorized')) {
          setStatus('unauthorized');
        } else {
          setStatus('error');
          setError(msg);
        }
      }
    }

    poll();
    const timer = setInterval(poll, 2000);
    return () => {
      alive = false;
      controller.abort();
      clearInterval(timer);
    };
  }, []);

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

  const checkCards = health
    ? Object.entries(health.checks).map(([name, check]) => (
        <Col span={8} key={name}>
          <Card size="small" title={name}>
            <Statistic title="Status" value={check.status} />
          </Card>
        </Col>
      ))
    : [];

  return (
    <Card title="Overview">
      <Row gutter={16}>
        <Col span={8}>
          <Card size="small">
            <Statistic title="Overall" value={health?.status ?? 'unknown'} />
          </Card>
        </Col>
        <Col span={8}>
          <Card size="small">
            <Statistic title="Uptime (s)" value={health?.uptime ?? 0} />
          </Card>
        </Col>
        <Col span={8}>
          <Card size="small">
            <Statistic title="Server" value={opsStatus?.server?.state ?? 'n/a'} />
          </Card>
        </Col>
      </Row>
      <Row gutter={16} style={{ marginTop: 16 }}>
        {checkCards}
      </Row>
    </Card>
  );
}
