import { useEffect, useRef, useState } from 'react';
import { Alert, Empty, Spin, Tag } from 'antd';
import { ProDescriptions, ProTable } from '@ant-design/pro-components';
import type { ProColumns } from '@ant-design/pro-components';
import { api } from '../services/api';
import type { ServiceDetail } from '../types';
import LoginGate from '../components/LoginGate';

interface ServiceRow {
  name: string;
}

// Fetch service names with the polling layer's timeout/cancel semantics.
function useServices() {
  const [state, setState] = useState<{
    loading: boolean; names: string[]; error: string | null; timedOut: boolean;
  }>({ loading: true, names: [], error: null, timedOut: false });

  useEffect(() => {
    const controller = new AbortController();
    setState({ loading: true, names: [], error: null, timedOut: false });
    api.services(controller.signal)
      .then((data) => {
        if (data === 'timeout') {
          setState({ loading: false, names: [], error: null, timedOut: true });
        } else {
          setState({ loading: false, names: data, error: null, timedOut: false });
        }
      })
      .catch((e) => {
        const msg = e instanceof Error ? e.message : String(e);
        const timedOut = msg.includes('timeout');
        setState({ loading: false, names: [], error: timedOut ? null : msg, timedOut });
      });
    return () => controller.abort();
  }, []);

  return state;
}

export default function ServicesPage() {
  const services = useServices();
  const [detail, setDetail] = useState<ServiceDetail | null>(null);
  const [detailError, setDetailError] = useState<string | null>(null);
  const [detailLoading, setDetailLoading] = useState(false);
  // Sequence guard: a late response from a previously selected service must
  // never overwrite a newer selection. The AbortController cancels in-flight
  // teardown; this ref orders the post-resolution commits.
  const detailSeq = useRef(0);
  const detailAbort = useRef<AbortController | null>(null);

  const columns: ProColumns<ServiceRow>[] = [
    {
      title: 'Service',
      dataIndex: 'name',
      render: (_, row) => (
        <a onClick={() => openDetail(row.name)}>{row.name}</a>
      ),
    },
  ];

  async function openDetail(name: string) {
    const seq = ++detailSeq.current;
    detailAbort.current?.abort();
    const controller = new AbortController();
    detailAbort.current = controller;
    setDetailLoading(true);
    setDetailError(null);
    setDetail(null);
    try {
      const d = await api.serviceDetail(name, controller.signal);
      if (seq !== detailSeq.current) return; // stale: newer selection owns UI
      setDetail(d);
    } catch (e) {
      if (seq !== detailSeq.current) return; // stale: drop
      const msg = e instanceof Error ? e.message : String(e);
      if (msg === 'Unauthorized') setDetailError('Unauthorized');
      else setDetailError(msg);
    } finally {
      if (seq === detailSeq.current) setDetailLoading(false);
    }
  }

  if (services.loading) {
    return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  }
  if (services.timedOut) {
    return <Alert type="warning" showIcon message="Services unavailable" description="The service list query timed out (2s); the actor mesh may be unresponsive." />;
  }
  if (services.error === 'Unauthorized') {
    return <LoginGate />;
  }
  if (services.error) {
    return <Alert type="error" showIcon message="Failed to load services" description={services.error} />;
  }
  if (services.names.length === 0) {
    return <Empty description="No services running" />;
  }

  return (
    <div>
      <ProTable<ServiceRow>
        rowKey="name"
        columns={columns}
        dataSource={services.names.map((n) => ({ name: n }))}
        search={false}
        options={false}
        pagination={false}
        headerTitle="Lua Services"
        toolBarRender={false}
      />
      {detailLoading && <Spin style={{ margin: 24 }} />}
      {detailError === 'Unauthorized' && <LoginGate />}
      {detailError && (
        <Alert style={{ marginTop: 16 }}
          type={detailError.includes('not found') ? 'error' : 'warning'}
          showIcon
          message={detailError.includes('not found') ? 'Service not found (404)' : 'Service query failed'}
          description={detailError}
        />
      )}
      {detail && (
        <ProDescriptions<ServiceDetail>
          title={detail.name}
          column={2}
          bordered
          style={{ marginTop: 16 }}
          dataSource={detail}
          columns={[
            { title: 'State', dataIndex: 'state', render: (_, d) => <Tag color={d.state === 'running' ? 'green' : 'orange'}>{d.state}</Tag> },
            { title: 'Uptime (s)', dataIndex: 'uptime_seconds' },
            { title: 'Timers', dataIndex: 'timers' },
            { title: 'Pending tasks', dataIndex: 'pending_tasks' },
            { title: 'Pending calls', dataIndex: 'pending_calls' },
            { title: 'Coroutines', dataIndex: 'coroutines' },
            { title: 'Memory (KB)', dataIndex: 'memory_kb' },
            { title: 'Requests', dataIndex: 'requests' },
            { title: 'Errors', dataIndex: 'errors' },
          ]}
        />
      )}
    </div>
  );
}
