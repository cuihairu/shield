import { useState } from 'react';
import { Navigate, Route, Routes, useNavigate } from 'react-router-dom';
import { ProLayout } from '@ant-design/pro-components';
import Dashboard from './pages/Dashboard';
import ServicesPage from './pages/ServicesPage';
import PluginsPage from './pages/PluginsPage';
import ConfigPage from './pages/ConfigPage';
import EvalProfilePage from './pages/EvalProfilePage';

const routes = [
  { path: '/', name: 'Overview', icon: 'dashboard' },
  { path: '/services', name: 'Services', icon: 'appstore' },
  { path: '/plugins', name: 'Plugins & Pools', icon: 'database' },
  { path: '/config', name: 'Config', icon: 'setting' },
  { path: '/eval-profile', name: 'Eval / Profile', icon: 'experiment' },
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
        <Route path="/plugins" element={<PluginsPage />} />
        <Route path="/config" element={<ConfigPage />} />
        <Route path="/eval-profile" element={<EvalProfilePage />} />
        <Route path="/ops/dashboard" element={<Navigate to="/" replace />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </ProLayout>
  );
}
