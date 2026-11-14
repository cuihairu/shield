import { describe, expect, it } from 'vitest';

import {
  EnvelopeDecoder,
  createEnvelopeDecoder,
  encodePacket,
  type Packet,
} from '../src/protocol/envelope.js';

describe('lenprefix envelope', () => {
  it('round-trips a packet (4-byte BE length, body only)', () => {
    const body = Buffer.from('{"route":"login"}');
    const wire = encodePacket({ routeId: 0, kind: 0, flags: 0, seq: 0, body }, { kind: 'lenprefix' });
    expect(wire.length).toBe(4 + body.length);
    expect(wire.readUInt32BE(0)).toBe(body.length);

    const dec = new EnvelopeDecoder({ kind: 'lenprefix' });
    const packets = dec.feed(wire);
    expect(packets).toHaveLength(1);
    expect(packets[0].body.toString()).toBe('{"route":"login"}');
    expect(packets[0].routeId).toBe(0);
    expect(dec.hasError).toBe(false);
    expect(dec.pendingBytes).toBe(0);
  });

  it('decodes a stream split at every byte boundary', () => {
    const bodies = ['alpha', 'beta', 'gamma'];
    const dec = new EnvelopeDecoder({ kind: 'lenprefix' });
    const wire = Buffer.concat(
      bodies.map((b) =>
        encodePacket({ routeId: 0, kind: 0, flags: 0, seq: 0, body: Buffer.from(b) }, { kind: 'lenprefix' }),
      ),
    );
    const packets: Packet[] = [];
    for (const byte of wire) {
      packets.push(...dec.feed(Buffer.from([byte])));
    }
    expect(packets.map((p) => p.body.toString())).toEqual(bodies);
  });

  it('honors lengthIncludesHeader', () => {
    const body = Buffer.from('x');
    // length field = 5 (4 header + 1 body)
    const wire = encodePacket({ routeId: 0, kind: 0, flags: 0, seq: 0, body }, {
      kind: 'lenprefix',
      lengthIncludesHeader: true,
    });
    expect(wire.readUInt32BE(0)).toBe(5);
    const dec = new EnvelopeDecoder({ kind: 'lenprefix', lengthIncludesHeader: true });
    const packets = dec.feed(wire);
    expect(packets).toHaveLength(1);
    expect(packets[0].body.toString()).toBe('x');
  });

  it('supports 2-byte little-endian widths', () => {
    const body = Buffer.from('hi');
    const wire = encodePacket({ routeId: 0, kind: 0, flags: 0, seq: 0, body }, {
      kind: 'lenprefix',
      lengthBytes: 2,
      endian: 'little',
    });
    expect(wire.length).toBe(2 + 2);
    expect(wire[0]).toBe(2);
    expect(wire[1]).toBe(0);
    const dec = new EnvelopeDecoder({ kind: 'lenprefix', lengthBytes: 2, endian: 'little' });
    expect(dec.feed(wire)[0].body.toString()).toBe('hi');
  });

  it('rejects an oversized frame and clears the buffer', () => {
    const dec = new EnvelopeDecoder({ kind: 'lenprefix', maxFrameSize: 8 });
    const evil = Buffer.alloc(4);
    evil.writeUInt32BE(64);
    const packets = dec.feed(Buffer.concat([evil, Buffer.from('junkjunkjunk')]));
    expect(packets).toHaveLength(0);
    expect(dec.hasError).toBe(true);
    expect(dec.error).toContain('too large');
    expect(dec.pendingBytes).toBe(0);
    // reset restores decoding
    dec.reset();
    const ok = encodePacket({ routeId: 0, kind: 0, flags: 0, seq: 0, body: Buffer.from('a') }, { kind: 'lenprefix' });
    expect(dec.feed(ok)).toHaveLength(1);
  });

  it('rejects an invalid width at construction', () => {
    const dec = new EnvelopeDecoder({ kind: 'lenprefix', lengthBytes: 3 });
    expect(dec.hasError).toBe(true);
    expect(dec.feed(Buffer.from([1, 2, 3]))).toHaveLength(0);
  });
});

describe('idlen envelope', () => {
  it('round-trips route id + length + body (engine hello_world framing: 2B BE route, 2B BE length)', () => {
    const body = Buffer.from('{"ok":1}');
    const routeId = 1;
    const wire = encodePacket({ routeId, kind: 0, flags: 0, seq: 0, body }, { kind: 'idlen' });
    expect(wire.length).toBe(2 + 2 + body.length);
    expect(wire.readUInt16BE(0)).toBe(routeId);
    expect(wire.readUInt16BE(2)).toBe(body.length);

    const dec = new EnvelopeDecoder({ kind: 'idlen' });
    const packets = dec.feed(wire);
    expect(packets).toHaveLength(1);
    expect(packets[0].routeId).toBe(routeId);
    expect(packets[0].body.toString()).toBe('{"ok":1}');
  });

  it('decodes two frames arriving in one chunk', () => {
    const wire = Buffer.concat([
      encodePacket({ routeId: 7, kind: 0, flags: 0, seq: 0, body: Buffer.from('a') }, { kind: 'idlen' }),
      encodePacket({ routeId: 9, kind: 0, flags: 0, seq: 0, body: Buffer.from('bb') }, { kind: 'idlen' }),
    ]);
    const dec = new EnvelopeDecoder({ kind: 'idlen' });
    const packets = dec.feed(wire);
    expect(packets.map((p) => p.routeId)).toEqual([7, 9]);
    expect(packets.map((p) => p.body.toString())).toEqual(['a', 'bb']);
  });

  it('keeps a partial frame buffered until the body arrives', () => {
    const wire = encodePacket({ routeId: 3, kind: 0, flags: 0, seq: 0, body: Buffer.from('hello') }, { kind: 'idlen' });
    const dec = new EnvelopeDecoder({ kind: 'idlen' });
    expect(dec.feed(wire.subarray(0, 3))).toHaveLength(0);
    expect(dec.pendingBytes).toBe(3);
    const packets = dec.feed(wire.subarray(3));
    expect(packets).toHaveLength(1);
    expect(packets[0].body.toString()).toBe('hello');
  });
});

describe('typed_len envelope', () => {
  it('sets kind to the low 16 bits of the type id (engine parity)', () => {
    const body = Buffer.from('payload');
    const typeId = 0x05;
    const wire = encodePacket({ routeId: typeId, kind: 0, flags: 0, seq: 0, body }, { kind: 'typed_len' });
    expect(wire.length).toBe(1 + 4 + body.length);

    const dec = new EnvelopeDecoder({ kind: 'typed_len' });
    const packets = dec.feed(wire);
    expect(packets).toHaveLength(1);
    expect(packets[0].routeId).toBe(typeId);
    expect(packets[0].kind).toBe(typeId);
  });

  it('defaults to 1-byte type and 4-byte length', () => {
    const wire = encodePacket({ routeId: 2, kind: 0, flags: 0, seq: 0, body: Buffer.from('') }, { kind: 'typed_len' });
    expect(wire.length).toBe(5);
    const dec = new EnvelopeDecoder({ kind: 'typed_len' });
    expect(dec.feed(wire)).toHaveLength(1);
  });
});

describe('delimiter envelope', () => {
  it('splits on the default newline and keeps the delimiter in rawFrame', () => {
    const dec = new EnvelopeDecoder({ kind: 'delimiter' });
    const packets = dec.feed(Buffer.from('one\ntwo\n'));
    expect(packets.map((p) => p.body.toString())).toEqual(['one', 'two']);
    expect(packets[0].rawFrame.toString()).toBe('one\n');
  });

  it('buffers an incomplete line', () => {
    const dec = new EnvelopeDecoder({ kind: 'delimiter' });
    expect(dec.feed(Buffer.from('partial'))).toHaveLength(0);
    const packets = dec.feed(Buffer.from(' rest\n'));
    expect(packets[0].body.toString()).toBe('partial rest');
  });

  it('errors on a frame larger than the cap', () => {
    const dec = new EnvelopeDecoder({ kind: 'delimiter', maxFrameSize: 4 });
    const packets = dec.feed(Buffer.from('0123456789'));
    expect(packets).toHaveLength(0);
    expect(dec.hasError).toBe(true);
  });

  it('honors a custom delimiter byte', () => {
    const dec = new EnvelopeDecoder({ kind: 'delimiter', delimiter: 0x00 });
    const packets = dec.feed(Buffer.from('a\x00b\x00'));
    expect(packets.map((p) => p.body.toString())).toEqual(['a', 'b']);
  });
});

describe('createEnvelopeDecoder', () => {
  it('accepts the engine config spellings', () => {
    expect(createEnvelopeDecoder('lenprefix')?.config.kind).toBe('lenprefix');
    expect(createEnvelopeDecoder('len_prefix')?.config.kind).toBe('lenprefix');
    expect(createEnvelopeDecoder('idlen')?.config.kind).toBe('idlen');
    expect(createEnvelopeDecoder('typed_len')?.config.kind).toBe('typed_len');
    expect(createEnvelopeDecoder('typelen')?.config.kind).toBe('typed_len');
    expect(createEnvelopeDecoder('line')?.config.kind).toBe('delimiter');
    expect(createEnvelopeDecoder('mystery')).toBeNull();
  });
});
