import assert from "node:assert/strict";
import crypto from "node:crypto";
import dgram from "node:dgram";
import net from "node:net";
import os from "node:os";
import { spawn } from "node:child_process";

const executable = process.argv[2];
if (!executable) {
  throw new Error("Relay executable path is required");
}

const extensionId = "abcdefghijklmnopabcdefghijklmnop";
const origin = `chrome-extension://${extensionId}`;
const receiverAddress = findPrivateIpv4();
const fakeReceiver = await startFakeReceiver(receiverAddress, "123456");
const child = spawn(executable, [origin], {
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true
});

let stderr = "";
child.stderr.setEncoding("utf8");
child.stderr.on("data", (chunk) => {
  stderr += chunk;
});

const timeout = setTimeout(() => {
  child.kill();
}, 10_000);

try {
  const ready = await readNativeMessage(child.stdout);
  assert.equal(ready.type, "ready");
  assert.equal(ready.protocol, 1);
  assert.ok(Number.isInteger(ready.port) && ready.port > 0);
  assert.match(ready.token, /^[0-9a-f]{64}$/);

  writeNativeMessage(child.stdin, {
    type: "configure_receiver",
    receiverAddress,
    pairingCode: "123456"
  });
  const receiverReady = await readNativeMessage(child.stdout);
  assert.deepEqual(receiverReady, { type: "receiver_ready", protocol: 2 });

  const socket = await connectSocket(ready.port);
  await performHandshake(socket, origin);
  const frames = createFrameReader(socket);

  socket.write(createClientFrame(
    0x1,
    Buffer.from(JSON.stringify({ type: "auth", token: ready.token }))
  ));
  const authenticated = JSON.parse((await frames.next()).payload.toString("utf8"));
  assert.deepEqual(authenticated, { type: "ready", protocol: 1 });
  const keyframeRequest = JSON.parse((await frames.next()).payload.toString("utf8"));
  assert.equal(keyframeRequest.type, "keyframe");
  assert.equal(keyframeRequest.requireCodecConfig, true);

  const payload = Buffer.alloc(137_003, 0x5a);
  payload.set([0x00, 0x00, 0x00, 0x01, 0x65], 0);
  const videoMessage = createLocalVideoMessage(payload);
  socket.write(createClientFrame(0x2, videoMessage.subarray(0, 65_536), false));
  socket.write(createClientFrame(0x0, videoMessage.subarray(65_536), true));
  socket.write(createClientFrame(
    0x1,
    Buffer.from(JSON.stringify({ type: "close" }))
  ));

  let telemetryFrame = await frames.next();
  while (
    telemetryFrame.opcode === 0x1 &&
    JSON.parse(telemetryFrame.payload.toString("utf8")).type === "keyframe"
  ) {
    telemetryFrame = await frames.next();
  }
  assert.equal(telemetryFrame.opcode, 0x1);
  const telemetry = JSON.parse(telemetryFrame.payload.toString("utf8"));
  assert.equal(telemetry.type, "telemetry");
  assert.equal(telemetry.receivedFrames, 1);
  assert.equal(telemetry.receivedBytes, payload.length);
  assert.equal(telemetry.keyFrames, 1);
  assert.equal(telemetry.invalidMessages, 0);
  assert.equal(telemetry.lanConnected, true);
  assert.equal(telemetry.lanSentFrames, 1);
  assert.equal(telemetry.lanSentBytes, payload.length);
  assert.ok(telemetry.lanSentDatagrams > 1);

  const receivedVideo = await fakeReceiver.video;
  assert.equal(receivedVideo.session, 0x12345678);
  assert.equal(receivedVideo.frame, 1);
  assert.equal(receivedVideo.payload.length, payload.length);
  assert.deepEqual(receivedVideo.payload, payload);

  const closeFrame = await frames.next();
  assert.equal(closeFrame.opcode, 0x8);
  socket.end();

  writeNativeMessage(child.stdin, { type: "shutdown" });
  const exitCode = await waitForExit(child);
  assert.equal(exitCode, 0, stderr);
  assert.doesNotMatch(stderr, new RegExp(ready.token));
  assert.doesNotMatch(stderr, /123456/);
  assert.doesNotMatch(stderr, /0123456789abcdef0123456789abcdef/);
  console.log("Relay Native Messaging + loopback WebSocket integration passed.");
} finally {
  clearTimeout(timeout);
  if (child.exitCode === null) {
    child.kill();
  }
  await fakeReceiver.close();
}

function findPrivateIpv4() {
  const candidates = [];
  for (const addresses of Object.values(os.networkInterfaces())) {
    for (const address of addresses ?? []) {
      if (address.family !== "IPv4" || address.internal) continue;
      const octets = address.address.split(".").map(Number);
      if (
        octets[0] === 10 ||
        (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31) ||
        (octets[0] === 192 && octets[1] === 168)
      ) {
        candidates.push(address.address);
      }
    }
  }
  candidates.sort((left, right) => {
    const rank = (value) => value.startsWith("192.168.") ? 0 :
      value.startsWith("10.") ? 1 : 2;
    return rank(left) - rank(right);
  });
  if (candidates.length > 0) return candidates[0];
  throw new Error("A private IPv4 address is required for the Relay integration test");
}

async function startFakeReceiver(address, pairingCode) {
  let resolveVideo;
  let rejectVideo;
  const video = new Promise((resolve, reject) => {
    resolveVideo = resolve;
    rejectVideo = reject;
  });
  const assemblies = new Map();
  const udp = dgram.createSocket("udp4");
  udp.on("message", (packet) => {
    try {
      assert.equal(packet.readUInt32BE(0), 0x48535332);
      assert.equal(packet.readUInt8(4), 2);
      assert.equal(packet.readUInt8(5), 32);
      assert.equal(packet.readUInt16BE(22), 0);
      const session = packet.readUInt32BE(8);
      const frame = packet.readUInt32BE(12);
      const fragment = packet.readUInt16BE(16);
      const fragments = packet.readUInt16BE(18);
      const payloadLength = packet.readUInt16BE(20);
      assert.equal(packet.length, 32 + payloadLength);
      const key = `${session}:${frame}`;
      const assembly = assemblies.get(key) ?? {
        session,
        frame,
        fragments,
        parts: new Array(fragments),
        count: 0
      };
      if (!assembly.parts[fragment]) {
        assembly.parts[fragment] = packet.subarray(32);
        assembly.count += 1;
      }
      assemblies.set(key, assembly);
      if (assembly.count === fragments) {
        assert.equal(packet.readUInt16BE(6) & 0x04, 0x04);
        resolveVideo({
          session,
          frame,
          payload: Buffer.concat(assembly.parts)
        });
      }
    } catch (error) {
      rejectVideo(error);
    }
  });
  const videoPort = await new Promise((resolve, reject) => {
    udp.once("error", reject);
    udp.bind(0, address, () => {
      udp.off("error", reject);
      resolve(udp.address().port);
    });
  });

  const clients = new Set();
  const tcp = net.createServer((socket) => {
    clients.add(socket);
    socket.on("close", () => clients.delete(socket));
    const controls = createControlReader(socket);
    void (async () => {
      const hello = await controls.next();
      assert.deepEqual(hello, { type: "hello", protocol: 2 });
      socket.write(createControlFrame({
        type: "hello",
        protocol: 2,
        receiverNonce: "00112233445566778899aabbccddeeff",
        pairingExpiresInSec: 300
      }));
      const pair = await controls.next();
      assert.equal(pair.type, "pair");
      assert.equal(pair.protocol, 2);
      assert.equal(pair.pairingCode, pairingCode);
      assert.equal(pair.receiverNonce, "00112233445566778899aabbccddeeff");
      socket.write(createControlFrame({
        type: "session",
        protocol: 2,
        sessionId: "0123456789abcdef0123456789abcdef",
        sessionShort: 0x12345678,
        codec: "video/avc",
        avcFormat: "annexb",
        width: 1280,
        height: 720,
        fps: 30,
        videoPort
      }));
      socket.write(createControlFrame({
        type: "keyframe",
        reason: "loss_flush_or_session_start",
        requireCodecConfig: true
      }));
    })().catch(rejectVideo);
  });
  await new Promise((resolve, reject) => {
    tcp.once("error", reject);
    tcp.listen(44000, address, () => {
      tcp.off("error", reject);
      resolve();
    });
  });

  return {
    video,
    async close() {
      for (const client of clients) client.destroy();
      await Promise.all([
        new Promise((resolve) => udp.close(resolve)),
        new Promise((resolve) => tcp.close(resolve))
      ]);
    }
  };
}

function createControlFrame(message) {
  const payload = Buffer.from(JSON.stringify(message), "utf8");
  const frame = Buffer.alloc(4 + payload.length);
  frame.writeUInt32BE(payload.length, 0);
  payload.copy(frame, 4);
  return frame;
}

function createControlReader(socket) {
  let buffer = Buffer.alloc(0);
  const queue = [];
  const waiting = [];
  const parse = () => {
    while (buffer.length >= 4) {
      const length = buffer.readUInt32BE(0);
      if (buffer.length < length + 4) return;
      const message = JSON.parse(
        buffer.subarray(4, length + 4).toString("utf8")
      );
      buffer = buffer.subarray(length + 4);
      const waiter = waiting.shift();
      if (waiter) waiter.resolve(message);
      else queue.push(message);
    }
  };
  socket.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    parse();
  });
  socket.on("error", (error) => {
    while (waiting.length > 0) waiting.shift().reject(error);
  });
  return {
    next() {
      if (queue.length > 0) return Promise.resolve(queue.shift());
      return new Promise((resolve, reject) => {
        waiting.push({ resolve, reject });
        parse();
      });
    }
  };
}

function readNativeMessage(stream) {
  return new Promise((resolve, reject) => {
    let buffer = Buffer.alloc(0);
    const onData = (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      if (buffer.length < 4) {
        return;
      }
      const length = buffer.readUInt32LE(0);
      if (length < 1 || length > 1024 * 1024) {
        cleanup();
        reject(new Error(`Invalid Native Messaging length: ${length}`));
        return;
      }
      if (buffer.length < 4 + length) {
        return;
      }
      cleanup();
      try {
        resolve(JSON.parse(buffer.subarray(4, 4 + length).toString("utf8")));
      } catch (error) {
        reject(error);
      }
    };
    const onEnd = () => {
      cleanup();
      reject(new Error(`Relay exited before ready: ${stderr}`));
    };
    const cleanup = () => {
      stream.off("data", onData);
      stream.off("end", onEnd);
    };
    stream.on("data", onData);
    stream.on("end", onEnd);
  });
}

function writeNativeMessage(stream, message) {
  const payload = Buffer.from(JSON.stringify(message), "utf8");
  const header = Buffer.alloc(4);
  header.writeUInt32LE(payload.length);
  stream.write(Buffer.concat([header, payload]));
}

function connectSocket(port) {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host: "127.0.0.1", port }, () => {
      socket.off("error", reject);
      resolve(socket);
    });
    socket.once("error", reject);
  });
}

function performHandshake(socket, expectedOrigin) {
  return new Promise((resolve, reject) => {
    const key = crypto.randomBytes(16).toString("base64");
    const expectedAccept = crypto
      .createHash("sha1")
      .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
      .digest("base64");
    socket.write([
      "GET /capture HTTP/1.1",
      "Host: 127.0.0.1",
      "Upgrade: websocket",
      "Connection: Upgrade",
      `Origin: ${expectedOrigin}`,
      "Sec-WebSocket-Version: 13",
      `Sec-WebSocket-Key: ${key}`,
      "",
      ""
    ].join("\r\n"));

    let response = Buffer.alloc(0);
    const onData = (chunk) => {
      response = Buffer.concat([response, chunk]);
      const end = response.indexOf("\r\n\r\n");
      if (end < 0) {
        return;
      }
      cleanup();
      const header = response.subarray(0, end).toString("utf8");
      assert.match(header, /^HTTP\/1\.1 101 Switching Protocols/m);
      assert.match(header, new RegExp(`Sec-WebSocket-Accept: ${escapeRegex(expectedAccept)}`, "i"));
      const remainder = response.subarray(end + 4);
      if (remainder.length > 0) {
        socket.unshift(remainder);
      }
      resolve();
    };
    const onError = (error) => {
      cleanup();
      reject(error);
    };
    const cleanup = () => {
      socket.off("data", onData);
      socket.off("error", onError);
    };
    socket.on("data", onData);
    socket.on("error", onError);
  });
}

function createClientFrame(opcode, payload, final = true) {
  const mask = crypto.randomBytes(4);
  const lengthBytes =
    payload.length <= 125 ? 0 :
      payload.length <= 0xffff ? 2 : 8;
  const frame = Buffer.alloc(2 + lengthBytes + 4 + payload.length);
  frame[0] = (final ? 0x80 : 0) | opcode;
  if (lengthBytes === 0) {
    frame[1] = 0x80 | payload.length;
  } else if (lengthBytes === 2) {
    frame[1] = 0x80 | 126;
    frame.writeUInt16BE(payload.length, 2);
  } else {
    frame[1] = 0x80 | 127;
    frame.writeBigUInt64BE(BigInt(payload.length), 2);
  }
  const maskOffset = 2 + lengthBytes;
  mask.copy(frame, maskOffset);
  for (let index = 0; index < payload.length; index += 1) {
    frame[maskOffset + 4 + index] = payload[index] ^ mask[index % 4];
  }
  return frame;
}

function createFrameReader(socket) {
  let buffer = Buffer.alloc(0);
  const waiting = [];

  const parse = () => {
    while (waiting.length > 0 && buffer.length >= 2) {
      const opcode = buffer[0] & 0x0f;
      let length = buffer[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (buffer.length < 4) return;
        length = buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (buffer.length < 10) return;
        length = Number(buffer.readBigUInt64BE(2));
        offset = 10;
      }
      if (buffer.length < offset + length) return;
      const payload = buffer.subarray(offset, offset + length);
      buffer = buffer.subarray(offset + length);
      waiting.shift().resolve({ opcode, payload });
    }
  };

  socket.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    parse();
  });
  socket.on("error", (error) => {
    while (waiting.length > 0) waiting.shift().reject(error);
  });
  socket.on("end", () => {
    while (waiting.length > 0) {
      waiting.shift().reject(new Error("WebSocket ended before the next frame"));
    }
  });

  return {
    next() {
      return new Promise((resolve, reject) => {
        waiting.push({ resolve, reject });
        parse();
      });
    }
  };
}

function createLocalVideoMessage(payload) {
  const header = Buffer.alloc(24);
  header.writeUInt32BE(0x48574c31, 0);
  header.writeUInt8(1, 4);
  header.writeUInt8(1, 5);
  header.writeUInt16BE(24, 6);
  header.writeUInt32BE(1, 8);
  header.writeUInt32BE(payload.length, 12);
  header.writeBigUInt64BE(1234567n, 16);
  return Buffer.concat([header, payload]);
}

function waitForExit(process) {
  return new Promise((resolve, reject) => {
    process.once("exit", resolve);
    process.once("error", reject);
  });
}

function escapeRegex(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
