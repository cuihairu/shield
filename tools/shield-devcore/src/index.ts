// [SHIELD_DEVCORE] Public surface. IDE-agnostic: no VS Code imports; a
// future Visual Studio shell consumes the same objects over plain Node.

export {
  DEFAULT_MAX_FRAME_SIZE,
  EnvelopeDecoder,
  createEnvelopeDecoder,
  encodePacket,
  normalizeEnvelopeConfig,
  type EnvelopeConfig,
  type EnvelopeKind,
  type Endian,
  type Packet,
} from './protocol/envelope.js';

export {
  ShieldTcpClient,
  type TcpTransportOptions,
  type TransportStatus,
} from './transport/tcpClient.js';

export {
  OpsHttpClient,
  OpsHttpError,
  parsePrometheusText,
  processGauges,
  type LuaServiceDetail,
  type OpsHealth,
  type OpsHttpOptions,
  type PromFamily,
  type PromSample,
  type PromSnapshot,
} from './sources/opsHttp.js';

export {
  LogTail,
  continuationRecord,
  parseShieldLogLine,
  type LogRecord,
  type LogLevel,
  type LogTailOptions,
} from './sources/logTail.js';

export {
  LuaServicesSource,
  resolveScriptPath,
  type LuaServiceEntry,
} from './sources/luaServices.js';

export {
  ProcSampler,
  parseProcStat,
  parseProcStatus,
  readProcSample,
  type ProcSample,
  type ProcSamplerOptions,
} from './sources/procSampler.js';

export {
  ShieldSession,
  type MetricsSample,
  type SessionOptions,
  type SessionStatus,
} from './session.js';
