import React from 'react';
import ReactDOM from 'react-dom/client';
import { ConfigProvider, App } from 'antd';
import AppShell from './App';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ConfigProvider>
      <App>
        <AppShell />
      </App>
    </ConfigProvider>
  </React.StrictMode>,
);
