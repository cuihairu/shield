import { useState } from 'react';
import { ProLayout } from '@ant-design/pro-components';
import Dashboard from './pages/Dashboard';

const routes = [
  {
    path: '/',
    name: 'Overview',
    icon: 'dashboard',
  },
];

export default function AppShell() {
  const [collapsed, setCollapsed] = useState(false);
  return (
    <ProLayout
      title="Shield Ops Console"
      logo={false}
      collapsed={collapsed}
      onCollapse={setCollapsed}
      route={{ path: '/', routes }}
      location={{ pathname: '/' }}
      menuItemRender={(_, dom) => dom}
    >
      <Dashboard />
    </ProLayout>
  );
}
