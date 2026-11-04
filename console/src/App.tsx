import { useState } from 'react';
import { Navigate, Route, Routes, useNavigate } from 'react-router-dom';
import { ProLayout } from '@ant-design/pro-components';
import Dashboard from './pages/Dashboard';
import ServicesPage from './pages/ServicesPage';

const routes = [
  { path: '/', name: 'Overview', icon: 'dashboard' },
  { path: '/services', name: 'Services', icon: 'appstore' },
];

export default function AppShell() {
  const [collapsed, setCollapsed] = useState(false);
  const nav = useNavigate();

  return (
    <ProLayout
      title="Shield Ops Console"
      logo={false}
      collapsed={collapsed}
      onCollapse={setCollapsed}
      route={{ path: '/', routes }}
      menuItemRender={(item, dom) => (
        <a
          onClick={() => {
            if (item.path) nav(item.path);
          }}
        >
          {dom}
        </a>
      )}
    >
      <Routes>
        <Route path="/" element={<Dashboard />} />
        <Route path="/services" element={<ServicesPage />} />
        <Route path="/ops/dashboard" element={<Navigate to="/" replace />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </ProLayout>
  );
}
