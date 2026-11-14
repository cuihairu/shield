// [SHIELD_DEVCORE] Wire-format frame envelopes, mirroring the engine's
// src/transport/protocol_envelope.cpp semantics: the same four framings,
// the same width/endian rules, the same max-frame guard and the same
// normalization of per-kind defaults. The devcore side only ever decodes
// (observe) and encodes for round-trip tests; it never injects traffic.

/// Default frame payload cap, mirroring kDefaultMaxFrameSize in
/// include/shield/transport/protocol.hpp (16 MiB).
export const DEFAULT_MAX_FRAME_SIZE = 16 * 1024 * 1024;

export type EnvelopeKind = 'lenprefix' | 'idlen' | 'typed_len' | 'delimiter';

export type Endian = 'big' | 'little';

export interface EnvelopeConfig {
  kind: EnvelopeKind;
  endian?: Endian;
  lengthBytes?: number;
  routeIdBytes?: number;
  /// When true the length field counts the header bytes as well (engine:
  /// length_includes_header).
  lengthIncludesHeader?: boolean;
  /// Byte value that terminates a delimiter frame (engine default '\n').
  delimiter?: number;
  /// 0 / unset means DEFAULT_MAX_FRAME_SIZE, never unlimited.
  maxFrameSize?: number;
}

export interface Packet {
  routeId: number;
  kind: number;
  flags: number;
  seq: number;
  body: Buffer;
  rawFrame: Buffer;
}

interface NormalizedConfig {
  kind: EnvelopeKind;
  endian: Endian;
  lengthBytes: number;
  routeIdBytes: number;
  lengthIncludesHeader: boolean;
  delimiter: number;
  maxFrameSize: number;
}

const VALID_WIDTHS = new Set([1, 2, 4, 8]);

export function normalizeEnvelopeConfig(config: EnvelopeConfig): NormalizedConfig {
  const endian = config.endian ?? 'big';
  const lengthIncludesHeader = config.lengthIncludesHeader ?? false;
  const maxFrameSize =
    config.maxFrameSize && config.maxFrameSize > 0
      ? config.maxFrameSize
      : DEFAULT_MAX_FRAME_SIZE;
  const delimiter = config.delimiter ?? 0x0a;

  // Per-kind defaults and forced fields mirror each envelope class ctor in
  // protocol_envelope.cpp: LenPrefix forces body-route, IdLen defaults
  // 2+2 bytes, TypeLen defaults 1+4 bytes, Delimiter clears both widths.
  switch (config.kind) {
    case 'lenprefix': {
      const lengthBytes = config.lengthBytes || 4;
      return {
        kind: config.kind,
        endian,
        lengthBytes,
        routeIdBytes: 0,
        lengthIncludesHeader,
        delimiter,
        maxFrameSize,
      };
    }
    case 'idlen': {
      return {
        kind: config.kind,
        endian,
        lengthBytes: config.lengthBytes || 2,
        routeIdBytes: config.routeIdBytes || 2,
        lengthIncludesHeader,
        delimiter,
        maxFrameSize,
      };
    }
    case 'typed_len': {
      return {
        kind: config.kind,
        endian,
        lengthBytes: config.lengthBytes || 4,
        routeIdBytes: config.routeIdBytes || 1,
        lengthIncludesHeader,
        delimiter,
        maxFrameSize,
      };
    }
    case 'delimiter': {
      return {
        kind: config.kind,
        endian,
        lengthBytes: 0,
        routeIdBytes: 0,
        lengthIncludesHeader,
        delimiter,
        maxFrameSize,
      };
    }
  }
}

function readUint(
  buf: Buffer,
  offset: number,
  width: number,
  endian: Endian,
): number {
  if (width === 8) {
    const big = endian === 'big' ? buf.readBigUInt64BE(offset) : buf.readBigUInt64LE(offset);
    // Frame sizes are capped far below 2^53, so the lossy Number cast of a
    // 64-bit length/route field is unreachable for valid traffic.
    return Number(big);
  }
  return endian === 'big'
    ? buf.readUIntBE(offset, width)
    : buf.readUIntLE(offset, width);
}

function writeUint(value: number, width: number, endian: Endian, out: Buffer): void {
  if (endian === 'big') {
    out.writeUIntBE(value, 0, width);
  } else {
    out.writeUIntLE(value, 0, width);
  }
}

function fitsWidth(value: number, width: number): boolean {
  if (width >= 8) {
    return true;
  }
  return value < 2 ** (width * 8);
}

function bodyLengthFromField(
  lengthField: number,
  headerSize: number,
  includesHeader: boolean,
): number | null {
  if (!includesHeader) {
    return lengthField;
  }
  if (lengthField < headerSize) {
    return null;
  }
  return lengthField - headerSize;
}

/// Streaming frame decoder. Feed arbitrary socket chunks; complete frames
/// come out as packets. On a protocol error (invalid width, oversized
/// frame, bad length) the decoder enters the error state, drops its buffer
/// and stops decoding until reset(), mirroring the engine envelopes.
export class EnvelopeDecoder {
  readonly config: NormalizedConfig;
  private buffer: Buffer = Buffer.alloc(0);
  private errorMessage = '';

  constructor(config: EnvelopeConfig) {
    this.config = normalizeEnvelopeConfig(config);
    if (!VALID_WIDTHS.has(this.config.lengthBytes) && this.config.kind !== 'delimiter') {
      this.errorMessage = `invalid ${this.config.kind} integer width`;
    }
    if (
      this.config.kind !== 'delimiter' &&
      this.config.kind !== 'lenprefix' &&
      !VALID_WIDTHS.has(this.config.routeIdBytes)
    ) {
      this.errorMessage = `invalid ${this.config.kind} integer width`;
    }
  }

  get error(): string {
    return this.errorMessage;
  }

  get hasError(): boolean {
    return this.errorMessage.length > 0;
  }

  get pendingBytes(): number {
    return this.buffer.length;
  }

  reset(): void {
    this.buffer = Buffer.alloc(0);
    this.errorMessage = '';
  }

  feed(chunk: Buffer): Packet[] {
    const packets: Packet[] = [];
    if (this.hasError) {
      return packets;
    }
    if (chunk.length > 0) {
      this.buffer = this.buffer.length === 0 ? chunk : Buffer.concat([this.buffer, chunk]);
    }

    switch (this.config.kind) {
      case 'delimiter':
        this.feedDelimiter(packets);
        break;
      case 'lenprefix':
        this.feedLengthFramed(packets, /* withRouteId */ false);
        break;
      case 'idlen':
      case 'typed_len':
        this.feedLengthFramed(packets, /* withRouteId */ true);
        break;
    }
    return packets;
  }

  private fail(message: string): void {
    this.errorMessage = message;
    this.buffer = Buffer.alloc(0);
  }

  private feedDelimiter(packets: Packet[]): void {
    while (true) {
      const idx = this.buffer.indexOf(this.config.delimiter);
      if (idx < 0) {
        if (this.buffer.length > this.config.maxFrameSize) {
          this.fail('delimiter frame too large');
        }
        return;
      }
      if (idx > this.config.maxFrameSize) {
        this.fail('delimiter frame too large');
        return;
      }
      packets.push({
        routeId: 0,
        kind: 0,
        flags: 0,
        seq: 0,
        body: Buffer.from(this.buffer.subarray(0, idx)),
        rawFrame: Buffer.from(this.buffer.subarray(0, idx + 1)),
      });
      this.buffer = this.buffer.subarray(idx + 1);
    }
  }

  private feedLengthFramed(packets: Packet[], withRouteId: boolean): void {
    const { endian, lengthBytes, routeIdBytes, lengthIncludesHeader, maxFrameSize } =
      this.config;
    const headerSize = (withRouteId ? routeIdBytes : 0) + lengthBytes;

    while (this.buffer.length >= headerSize) {
      let routeId = 0;
      if (withRouteId) {
        routeId = readUint(this.buffer, 0, routeIdBytes, endian);
      }
      const lengthField = readUint(this.buffer, withRouteId ? routeIdBytes : 0, lengthBytes, endian);

      const maybePayloadLen = bodyLengthFromField(lengthField, headerSize, lengthIncludesHeader);
      if (maybePayloadLen === null) {
        this.fail(`${this.config.kind} frame length overflow`);
        return;
      }
      const payloadLen = maybePayloadLen;
      if (payloadLen > maxFrameSize) {
        this.fail(`${this.config.kind} frame too large`);
        return;
      }
      const totalLen = payloadLen + headerSize;
      if (this.buffer.length < totalLen) {
        return;
      }

      const body = Buffer.from(this.buffer.subarray(headerSize, totalLen));
      packets.push({
        routeId,
        // typed_len: kind is the low 16 bits of the type id, exactly like
        // TypeLenEnvelope::feed in the engine.
        kind:
          this.config.kind === 'typed_len' ? routeId & 0xffff : 0,
        flags: 0,
        seq: 0,
        body,
        rawFrame: Buffer.from(this.buffer.subarray(0, totalLen)),
      });
      this.buffer = this.buffer.subarray(totalLen);
    }
  }
}

/// Encode one packet into the wire framing (engine encode() parity). Used
/// by tests and by future devcore-facing push sources; the plugin itself
/// stays read-only on the wire.
export function encodePacket(packet: Omit<Packet, 'rawFrame'>, config: EnvelopeConfig): Buffer {
  const norm = normalizeEnvelopeConfig(config);
  switch (norm.kind) {
    case 'delimiter': {
      const out = Buffer.alloc(packet.body.length + 1);
      packet.body.copy(out, 0);
      out[out.length - 1] = norm.delimiter;
      return out;
    }
    case 'lenprefix': {
      const lengthField = norm.lengthIncludesHeader
        ? packet.body.length + norm.lengthBytes
        : packet.body.length;
      if (!VALID_WIDTHS.has(norm.lengthBytes) || !fitsWidth(lengthField, norm.lengthBytes)) {
        throw new Error('lenprefix payload length does not fit header');
      }
      const out = Buffer.alloc(norm.lengthBytes + packet.body.length);
      writeUint(lengthField, norm.lengthBytes, norm.endian, out);
      packet.body.copy(out, norm.lengthBytes);
      return out;
    }
    case 'idlen':
    case 'typed_len': {
      const typeId = norm.kind === 'typed_len' && packet.routeId === 0 ? packet.kind : packet.routeId;
      const lengthField = norm.lengthIncludesHeader
        ? packet.body.length + norm.routeIdBytes + norm.lengthBytes
        : packet.body.length;
      if (
        !VALID_WIDTHS.has(norm.routeIdBytes) ||
        !VALID_WIDTHS.has(norm.lengthBytes) ||
        !fitsWidth(typeId, norm.routeIdBytes) ||
        !fitsWidth(lengthField, norm.lengthBytes)
      ) {
        throw new Error(`${norm.kind} header field does not fit`);
      }
      const out = Buffer.alloc(norm.routeIdBytes + norm.lengthBytes + packet.body.length);
      writeUint(typeId, norm.routeIdBytes, norm.endian, out);
      writeUint(lengthField, norm.lengthBytes, norm.endian, out.subarray(norm.routeIdBytes));
      packet.body.copy(out, norm.routeIdBytes + norm.lengthBytes);
      return out;
    }
  }
}

export function createEnvelopeDecoder(kind: string, config: Omit<EnvelopeConfig, 'kind'> = {}) {
  switch (kind) {
    case 'lenprefix':
    case 'len-prefix':
    case 'len_prefix':
      return new EnvelopeDecoder({ ...config, kind: 'lenprefix' });
    case 'idlen':
    case 'id-len':
    case 'id_len':
      return new EnvelopeDecoder({ ...config, kind: 'idlen' });
    case 'typed_len':
    case 'type_len':
    case 'typed-len':
    case 'typelen':
      return new EnvelopeDecoder({ ...config, kind: 'typed_len' });
    case 'delimiter':
    case 'line':
      return new EnvelopeDecoder({ ...config, kind: 'delimiter' });
    default:
      return null;
  }
}
