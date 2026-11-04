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
  uptime_seconds?: number;
  name?: string;
  watchers?: number;
  shutdown_scheduled?: boolean;
}

export interface ClusterNode {
  node_id: string;
  address?: string;
  state?: string;
  epoch?: string;
  last_heartbeat_ms?: number;
  heartbeat_age_ms?: number;
}

export interface ClusterStatus {
  node_id?: string;
  node_epoch?: string;
  nodes: ClusterNode[];
}

export interface OpsStatusData {
  server?: ServerState;
  services?: string[] | 'timeout';
  cluster?: ClusterStatus;
}

export interface ServiceDetail {
  name: string;
  state: string;
  script?: string;
  rpc_routes?: string[];
  requests?: number;
  errors?: number;
  uptime_seconds?: number;
  timers?: number;
  pending_calls?: number;
  pending_tasks?: number;
  coroutines?: number;
  memory_kb?: number;
}
