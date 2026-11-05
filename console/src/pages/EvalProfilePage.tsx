import { useEffect, useState } from 'react';
import { Alert, Button, Card, Input, InputNumber, Space, Spin, Typography } from 'antd';
import { api } from '../services/api';
import { HttpError } from '../services/polling';
import LoginGate from '../components/LoginGate';

interface ControlState {
  evalEnabled: boolean;
  profileEnabled: boolean;
  cooldownSeconds: number | null;
  loading: boolean;
  error: string | null;
  unauthorized: boolean;
}

// FIX-WEB-12: eval/profile 受控操作页. The endpoint availability is decided
// by the server-side opt-in config; the page never invents a form when the
// endpoint is not registered (it would 404 otherwise). Tokens are typed
// manually per operation and kept only in component state (never persisted).
export default function EvalProfilePage() {
  const [cfg, setCfg] = useState<ControlState>({
    evalEnabled: false,
    profileEnabled: false,
    cooldownSeconds: null,
    loading: true,
    error: null,
    unauthorized: false,
  });

  // Per-operation token fields (never written to localStorage/sessionStorage).
  const [evalToken, setEvalToken] = useState('');
  const [profileToken, setProfileToken] = useState('');
  const [code, setCode] = useState('return 1');
  const [evalResult, setEvalResult] = useState<string | null>(null);
  const [evalError, setEvalError] = useState<string | null>(null);
  const [evalBusy, setEvalBusy] = useState(false);

  const [profileService, setProfileService] = useState('');
  const [profileDuration, setProfileDuration] = useState(5000);
  const [profileOutput, setProfileOutput] = useState<string | null>(null);
  const [profileError, setProfileError] = useState<string | null>(null);
  const [profileBusy, setProfileBusy] = useState(false);

  useEffect(() => {
    const controller = new AbortController();
    Promise.all([
      api.config('http.eval_enabled', controller.signal),
      api.config('http.eval_token', controller.signal),
      api.config('http.profile_enabled', controller.signal),
      api.config('http.profile_token', controller.signal),
      api.config('http.profile_cooldown_seconds', controller.signal),
    ])
      .then(([evalOn, evalTok, profOn, profTok, cool]) => {
        // The server registers the endpoint only when BOTH the enabled flag
        // and a non-empty token are present. The config endpoint returns
        // null for an empty token and the redaction marker for a configured
        // one, so a non-null value here means the endpoint is actually live.
        const evalLive = evalOn === 'true' && evalTok != null;
        const profileLive = profOn === 'true' && profTok != null;
        setCfg({
          evalEnabled: evalLive,
          profileEnabled: profileLive,
          cooldownSeconds: cool == null ? null : Number(cool),
          loading: false,
          error: null,
          unauthorized: false,
        });
      })
      .catch((e) => {
        const msg = e instanceof Error ? e.message : String(e);
        setCfg((c) => ({
          ...c,
          loading: false,
          unauthorized: msg === 'Unauthorized',
          error: msg === 'Unauthorized' ? null : msg,
        }));
      });
    return () => controller.abort();
  }, []);

  async function runEval() {
    if (!evalToken.trim() || !code.trim()) return;
    setEvalBusy(true);
    setEvalResult(null);
    setEvalError(null);
    try {
      const data = await api.evalCode(code, evalToken.trim());
      setEvalResult(JSON.stringify(data));
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      setEvalError(msg === 'Unauthorized' ? 'Invalid or missing eval token' : msg);
    } finally {
      setEvalBusy(false);
    }
  }

  async function runProfile(action: string) {
    if (!profileToken.trim()) return;
    if (action === 'start' && !profileService.trim()) {
      setProfileError('service is required to start a profile session');
      return;
    }
    setProfileBusy(true);
    setProfileOutput(null);
    setProfileError(null);
    try {
      const body: Record<string, unknown> = { action };
      if (action === 'start') {
        body.service = profileService.trim();
        body.duration_ms = profileDuration;
      }
      const data = await api.profile(action, body, profileToken.trim());
      setProfileOutput(JSON.stringify(data));
    } catch (e) {
      let msg = e instanceof Error ? e.message : String(e);
      if (e instanceof HttpError) {
        if (e.status === 409) msg = `409: a profile session is already running`;
        else if (e.status === 429) msg = `429: start cooldown not elapsed`;
        else if (e.status === 504) msg = `504: the profile request timed out`;
        else if (e.status === 401) msg = `Invalid or missing profile token`;
      } else if (msg === 'Unauthorized') {
        msg = 'Invalid or missing profile token';
      }
      setProfileError(msg);
    } finally {
      setProfileBusy(false);
    }
  }

  if (cfg.loading) {
    return <Spin size="large" style={{ display: 'block', margin: '80px auto' }} />;
  }
  if (cfg.unauthorized) {
    return <LoginGate />;
  }
  if (cfg.error) {
    return <Alert type="error" showIcon message="Failed to load control config" description={cfg.error} />;
  }

  return (
    <div>
      <Alert type="info" showIcon style={{ marginBottom: 16 }}
        message="Controlled operations"
        description="eval/profile endpoints are opt-in server-side. Tokens are typed per session and never persisted." />
      <Space direction="vertical" size="large" style={{ width: '100%' }}>
        <Card title="Eval (remote code entry)" size="small">
          {!cfg.evalEnabled ? (
            <Alert type="warning" showIcon
              message="Endpoint not enabled"
              description="http.eval_enabled is not true, or http.eval_token is empty. Ask an operator to opt in." />
          ) : (
            <Space direction="vertical" style={{ width: '100%' }}>
              <Input.Password placeholder="eval token (manual, not persisted)"
                value={evalToken} onChange={(e) => setEvalToken(e.target.value)}
                autoComplete="off" />
              <Input.TextArea placeholder="Lua code, e.g. return 6 * 7"
                value={code} onChange={(e) => setCode(e.target.value)} rows={4}
                style={{ fontFamily: 'monospace' }} />
              <Button type="primary" danger onClick={runEval} loading={evalBusy}
                disabled={!evalToken.trim() || !code.trim()}>
                Run eval (restricted VM)
              </Button>
              {evalResult && <pre style={{ background: '#f6f6f6', padding: 8 }}>{evalResult}</pre>}
              {evalError && <Alert type="error" showIcon message={evalError} />}
            </Space>
          )}
        </Card>

        <Card title="Profiler" size="small">
          {!cfg.profileEnabled ? (
            <Alert type="warning" showIcon
              message="Endpoint not enabled"
              description="http.profile_enabled is not true, or http.profile_token is empty. Ask an operator to opt in." />
          ) : (
            <Space direction="vertical" style={{ width: '100%' }}>
              <Input.Password placeholder="profile token (manual, not persisted)"
                value={profileToken} onChange={(e) => setProfileToken(e.target.value)}
                autoComplete="off" />
              <Input placeholder="service id (required for start)"
                value={profileService} onChange={(e) => setProfileService(e.target.value)} />
              <InputNumber min={1} max={60000} value={profileDuration}
                onChange={(v) => setProfileDuration(v ?? 5000)}
                addonAfter="duration_ms" />
              {cfg.cooldownSeconds != null && (
                <Typography.Text type="secondary">
                  Start cooldown: {cfg.cooldownSeconds}s
                </Typography.Text>
              )}
              <Space>
                <Button onClick={() => runProfile('status')} loading={profileBusy}>Status</Button>
                <Button type="primary" onClick={() => runProfile('start')} loading={profileBusy}
                  disabled={!profileService.trim()}>Start</Button>
                <Button onClick={() => runProfile('report')} loading={profileBusy}>Stop & report</Button>
              </Space>
              {profileOutput && <pre style={{ background: '#f6f6f6', padding: 8 }}>{profileOutput}</pre>}
              {profileError && <Alert type="error" showIcon message={profileError} />}
            </Space>
          )}
        </Card>
      </Space>
    </div>
  );
}
