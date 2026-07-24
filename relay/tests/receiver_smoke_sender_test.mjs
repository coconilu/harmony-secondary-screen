import assert from 'node:assert/strict';
import test from 'node:test';

import {
  encodeControl,
  fragmentAccessUnit,
} from '../tools/receiver_smoke_sender.mjs';

test('control frame uses a big-endian length prefix', () => {
  const frame = encodeControl({ type: 'hello', protocol: 2 });
  assert.equal(frame.readUInt32BE(0), frame.length - 4);
  assert.deepEqual(JSON.parse(frame.subarray(4).toString('utf8')),
    { type: 'hello', protocol: 2 });
});

test('HSS2 fragmentation marks only the final datagram as end-of-frame', () => {
  const payload = Buffer.alloc(2500, 0x5a);
  const packets = fragmentAccessUnit(payload, 0x12345678, 7, 99n);
  assert.equal(packets.length, 3);
  assert.deepEqual(packets.map((packet) => packet.length), [1232, 1232, 132]);
  for (let index = 0; index < packets.length; index += 1) {
    const packet = packets[index];
    assert.equal(packet.readUInt32BE(0), 0x48535332);
    assert.equal(packet.readUInt8(4), 2);
    assert.equal(packet.readUInt8(5), 32);
    assert.equal(packet.readUInt16BE(6), index === 2 ? 0x07 : 0x03);
    assert.equal(packet.readUInt32BE(8), 0x12345678);
    assert.equal(packet.readUInt32BE(12), 7);
    assert.equal(packet.readUInt16BE(16), index);
    assert.equal(packet.readUInt16BE(18), 3);
    assert.equal(packet.readUInt16BE(22), 0);
    assert.equal(packet.readBigUInt64BE(24), 99n);
  }
});

test('invalid session and oversized access units are rejected', () => {
  assert.throws(() => fragmentAccessUnit(Buffer.from([1]), 0), /sessionShort/);
  assert.throws(() => fragmentAccessUnit(Buffer.alloc(8 * 1024 * 1024 + 1), 1), /1..8 MiB/);
});
