import { useState } from 'react';
import { Alert, Input, Button, Space, Typography } from 'antd';
import { setOpsToken, getOpsToken } from '../services/polling';

/**
 * Bearer-token entry point for the ops auth gate. The token is kept in
 * localStorage (never persisted server-side) and attached to every /ops
 * request by the polling layer. When the gate denies a request the console
 * renders this gate instead of the page content.
 */
export default function LoginGate() {
  const [value, setValue] = useState('');
  const [saved, setSaved] = useState(!!getOpsToken());

  async function submit() {
    setOpsToken(value.trim());
    setSaved(true);
    // Reload so every polling hook picks up the new token at mount.
    window.location.reload();
  }

  return (
    <div style={{ maxWidth: 420, margin: '80px auto' }}>
      <Alert
        type="warning"
        showIcon
        style={{ marginBottom: 16 }}
        message="Authentication required"
        description="The ops server requires a Bearer token. Paste it once; it is kept only in this browser's localStorage."
      />
      <Space.Compact style={{ width: '100%' }}>
        <Input.Password
          placeholder="Ops token"
          value={value}
          onChange={(e) => setValue(e.target.value)}
          onPressEnter={submit}
          autoComplete="off"
        />
        <Button type="primary" onClick={submit} disabled={!value.trim()}>
          Sign in
        </Button>
      </Space.Compact>
      {saved && (
        <Typography.Text type="secondary" style={{ display: 'block', marginTop: 12 }}>
          Token saved locally. If the token is wrong the gate will re-appear on next 401.
        </Typography.Text
        >
      )}
    </div>
  );
}
