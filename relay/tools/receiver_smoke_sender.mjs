import crypto from 'node:crypto';
import dgram from 'node:dgram';
import fs from 'node:fs/promises';
import net from 'node:net';
import process from 'node:process';
import { pathToFileURL } from 'node:url';

const CONTROL_PORT = 44000;
const VIDEO_MAGIC = 0x48535332;
const VIDEO_VERSION = 2;
const VIDEO_PORT = 47101;
const VIDEO_HEADER_SIZE = 32;
const MAX_UDP_PAYLOAD = 1200;
const MAX_FRAME_BYTES = 8 * 1024 * 1024;

export function encodeControl(value) {
  const payload = Buffer.from(JSON.stringify(value), 'utf8');
  if (payload.length === 0 || payload.length > 64 * 1024) {
    throw new Error('control payload is outside 1..65536 bytes');
  }
  const frame = Buffer.allocUnsafe(4 + payload.length);
  frame.writeUInt32BE(payload.length, 0);
  payload.copy(frame, 4);
  return frame;
}

export function fragmentAccessUnit(payload, session, frame = 1, timestampUs = 0n) {
  if (!Buffer.isBuffer(payload) || payload.length === 0 || payload.length > MAX_FRAME_BYTES) {
    throw new Error('H.264 access unit must be 1..8 MiB');
  }
  if (!Number.isInteger(session) || session <= 0 || session > 0xffffffff) {
    throw new Error('sessionShort must be a non-zero uint32');
  }
  const fragments = Math.ceil(payload.length / MAX_UDP_PAYLOAD);
  if (fragments > 0xffff) throw new Error('fragment count exceeds uint16');

  const datagrams = [];
  for (let fragment = 0; fragment < fragments; fragment += 1) {
    const start = fragment * MAX_UDP_PAYLOAD;
    const chunk = payload.subarray(start, Math.min(start + MAX_UDP_PAYLOAD, payload.length));
    const packet = Buffer.alloc(VIDEO_HEADER_SIZE + chunk.length);
    const flags = 0x01 | 0x02 | (fragment === fragments - 1 ? 0x04 : 0);
    packet.writeUInt32BE(VIDEO_MAGIC, 0);
    packet.writeUInt8(VIDEO_VERSION, 4);
    packet.writeUInt8(VIDEO_HEADER_SIZE, 5);
    packet.writeUInt16BE(flags, 6);
    packet.writeUInt32BE(session, 8);
    packet.writeUInt32BE(frame >>> 0, 12);
    packet.writeUInt16BE(fragment, 16);
    packet.writeUInt16BE(fragments, 18);
    packet.writeUInt16BE(chunk.length, 20);
    packet.writeUInt16BE(0, 22);
    packet.writeBigUInt64BE(BigInt(timestampUs), 24);
    chunk.copy(packet, VIDEO_HEADER_SIZE);
    datagrams.push(packet);
  }
  return datagrams;
}

class ControlReader {
  constructor(socket) {
    this.buffer = Buffer.alloc(0);
    this.queue = [];
    this.waiters = [];
    socket.on('data', (data) => this.push(data));
    socket.on('error', (error) => this.fail(error));
    socket.on('close', () => this.fail(new Error('control connection closed')));
  }

  push(data) {
    this.buffer = Buffer.concat([this.buffer, data]);
    while (this.buffer.length >= 4) {
      const length = this.buffer.readUInt32BE(0);
      if (length === 0 || length > 64 * 1024) {
        this.fail(new Error('receiver sent an invalid control frame length'));
        return;
      }
      if (this.buffer.length < length + 4) return;
      const payload = this.buffer.subarray(4, length + 4);
      this.buffer = this.buffer.subarray(length + 4);
      let message;
      try {
        message = JSON.parse(payload.toString('utf8'));
      } catch {
        this.fail(new Error('receiver sent invalid JSON'));
        return;
      }
      const waiter = this.waiters.shift();
      if (waiter) waiter.resolve(message);
      else this.queue.push(message);
    }
  }

  fail(error) {
    for (const waiter of this.waiters.splice(0)) waiter.reject(error);
  }

  next(timeoutMs) {
    if (this.queue.length > 0) return Promise.resolve(this.queue.shift());
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject };
      this.waiters.push(waiter);
      const timer = setTimeout(() => {
        const index = this.waiters.indexOf(waiter);
        if (index >= 0) this.waiters.splice(index, 1);
        reject(new Error(`control response timed out after ${timeoutMs} ms`));
      }, timeoutMs);
      waiter.resolve = (value) => {
        clearTimeout(timer);
        resolve(value);
      };
      waiter.reject = (error) => {
        clearTimeout(timer);
        reject(error);
      };
    });
  }
}

function connectControl(address) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: address, port: CONTROL_PORT });
    socket.setNoDelay(true);
    socket.once('connect', () => resolve(socket));
    socket.once('error', reject);
  });
}

function sendDatagram(socket, packet, address, port) {
  return new Promise((resolve, reject) => {
    socket.send(packet, port, address, (error) => error ? reject(error) : resolve());
  });
}

function assertIpv4(value) {
  if (net.isIPv4(value) === false || value === '0.0.0.0' || value.startsWith('127.')) {
    throw new Error('receiver address must be a concrete non-loopback IPv4');
  }
}

export async function runSmoke({ address, pairingCode, h264Path }) {
  assertIpv4(address);
  if (!/^\d{6}$/.test(pairingCode)) throw new Error('pairing code must contain six digits');
  const accessUnit = await fs.readFile(h264Path);
  if (accessUnit.length === 0 || accessUnit.length > MAX_FRAME_BYTES) {
    throw new Error('H.264 access unit must be 1..8 MiB');
  }

  const control = await connectControl(address);
  const reader = new ControlReader(control);
  try {
    control.write(encodeControl({ type: 'hello', protocol: 2 }));
    const hello = await reader.next(3000);
    if (hello.type !== 'hello' || hello.protocol !== 2 ||
        typeof hello.receiverNonce !== 'string') {
      throw new Error('receiver returned an unexpected hello response');
    }

    control.write(encodeControl({
      type: 'pair',
      protocol: 2,
      pairingCode,
      senderNonce: crypto.randomBytes(16).toString('hex'),
      receiverNonce: hello.receiverNonce,
      codec: 'video/avc',
      avcFormat: 'annexb',
      width: 1280,
      height: 720,
      fps: 30,
    }));
    const session = await reader.next(3000);
    if (session.type === 'error') {
      throw new Error(`pairing rejected: ${String(session.code ?? 'unknown_error')}`);
    }
    if (session.type !== 'session' || session.protocol !== 2 ||
        !Number.isInteger(session.sessionShort) || session.sessionShort <= 0 ||
        session.videoPort !== VIDEO_PORT) {
      throw new Error(`pairing rejected with control type: ${String(session.type ?? 'unknown')}`);
    }

    const udp = dgram.createSocket('udp4');
    try {
      const heartbeat = setInterval(() => {
        if (!control.destroyed) {
          control.write(encodeControl({
            type: 'ping',
            senderSendUs: Number(process.hrtime.bigint() / 1000n),
          }));
        }
      }, 1000);
      const timestampUs = process.hrtime.bigint() / 1000n;
      const datagrams = fragmentAccessUnit(accessUnit, session.sessionShort, 1, timestampUs);
      try {
        for (const datagram of datagrams) {
          await sendDatagram(udp, datagram, address, session.videoPort);
        }
        const deadline = Date.now() + 8000;
        while (Date.now() < deadline) {
          const message = await reader.next(Math.max(1, deadline - Date.now()));
          if (message.type === 'telemetry' && message.framesDecoded >= 1) {
            control.write(encodeControl({ type: 'stop', reason: 'smoke_test_complete' }));
            return {
              framesDecoded: message.framesDecoded,
              framesDropped: message.framesDropped,
              bytes: accessUnit.length,
              fragments: datagrams.length,
            };
          }
          if (message.type === 'error') {
            throw new Error(`receiver error: ${String(message.code ?? 'unknown')}`);
          }
        }
        throw new Error('receiver did not report a decoded frame');
      } finally {
        clearInterval(heartbeat);
      }
    } finally {
      udp.close();
    }
  } finally {
    control.destroy();
  }
}

async function main() {
  const [, , address, pairingCode, h264Path] = process.argv;
  if (!address || !pairingCode || !h264Path) {
    console.error('Usage: node receiver_smoke_sender.mjs <receiver-ipv4> <pairing-code> <annexb.h264>');
    process.exitCode = 2;
    return;
  }
  const result = await runSmoke({ address, pairingCode, h264Path });
  console.log(JSON.stringify({ ok: true, ...result }));
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
  main().catch((error) => {
    console.error(`Receiver smoke failed: ${error.message}`);
    process.exitCode = 1;
  });
}
