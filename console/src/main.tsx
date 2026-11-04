import React from 'react';
import ReactDOM from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import { ConfigProvider, App } from 'antd';
import AppShell from './App';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ConfigProvider>
      <App>
        <BrowserRouter>
          <AppShell />
        </BrowserRouter>
      </App>
    </ConfigProvider>
  </React.StrictMode>,
);
