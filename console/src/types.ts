export interface Envelope<T> {
  type: 'result' | 'error';
  data: T;
}

export interface HealthCheck {
  status: string;
  uptime_seconds?: number;
  started?: number;
  required_down?: number;
  state?: string;
  nodes_online?: number;
  nodes_down?: number;
}

export interface HealthData {
  status: string;
  uptime: number;
  checks: Record<string, HealthCheck>;
}

export interface ServerState {
  state: string;
  uptime: number;
  version?: string;
  watchers?: number;
  services?: Record<string, { state: string; uptime: number }>;
}

export interface OpsStatusData {
  server?: ServerState;
  services?: Record<string, { state: string; uptime: number }>;
  cluster?: { nodes_online: number; nodes_down: number };
}
