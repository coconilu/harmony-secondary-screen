import test from "node:test";
import assert from "node:assert/strict";
import { once } from "node:events";
import { WebSocket, WebSocketServer } from "ws";

import { DirectReceiverConnection, pairReceiver } from "../direct-client.js";
import { createDirectVideoMessage } from "../direct-protocol.js";

globalThis.WebSocket = WebSocket;

test("QR authorization pairs with a fake Receiver and returns stable credentials", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  server.once("connection", (socket) => {
    socket.once("message", (data) => {
      const pair = JSON.parse(data.toString("utf8"));
      assert.equal(pair.type, "pair");
      assert.equal(pair.sessionId, "1".repeat(32));
      assert.equal(pair.token, "2".repeat(64));
      socket.send(JSON.stringify({
        type: "paired",
        protocol: 4,
        deviceId: "00112233445566778899aabbccddeeff",
        credential: "a".repeat(64)
      }));
    });
  });
  const trusted = await pairReceiver({
    host: "192.168.1.8",
    authorization: {
      sessionId: "1".repeat(32),
      token: "2".repeat(64)
    },
    senderId: "019fa3cf-75c7-7000-8000-000000000001",
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`)
  });
  assert.equal(trusted.deviceId, "00112233445566778899aabbccddeeff");
  assert.equal(trusted.credential, "a".repeat(64));
});

test("direct WebSocket authenticates and delivers one Annex-B AU to a fake Receiver", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");

  const annexB = Uint8Array.from([
    0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
    0, 0, 0, 1, 0x65, 0x01, 0x02, 0x03
  ]);
  const received = new Promise((resolve, reject) => {
    server.once("connection", (socket) => {
      socket.once("message", (data, isBinary) => {
        try {
          assert.equal(isBinary, false);
          const auth = JSON.parse(data.toString("utf8"));
          assert.equal(auth.type, "auth");
          assert.equal(auth.protocol, 4);
          assert.equal(auth.sourceEpoch, 7);
          assert.equal(auth.width, 1280);
          assert.equal(auth.height, 720);
          assert.equal(auth.fps, 60);
          socket.send(JSON.stringify({
            type: "ready",
            protocol: 4,
            sourceEpoch: 7
          }));
          socket.once("message", (video, videoIsBinary) => {
            try {
              assert.equal(videoIsBinary, true);
              resolve(Uint8Array.from(video));
            } catch (error) {
              reject(error);
            }
          });
        } catch (error) {
          reject(error);
        }
      });
    });
  });

  const connection = new DirectReceiverConnection({
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 7,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    heartbeatIntervalMs: 60_000
  });
  await connection.connect();

  const wireMessage = createDirectVideoMessage({
    byteLength: annexB.byteLength,
    timestamp: 55,
    type: "key",
    copyTo(destination) {
      destination.set(annexB);
    }
  }, 7, 3);
  assert.equal(connection.sendVideo(wireMessage), true);
  assert.deepEqual(await received, new Uint8Array(wireMessage));
  await connection.close();
});

test("direct WebSocket recovers an abnormal drop without replacing the capture source", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");

  const authEpochs = [];
  let connectionCount = 0;
  let resolveRecoveredVideo;
  const recoveredVideo = new Promise((resolve) => {
    resolveRecoveredVideo = resolve;
  });
  server.on("connection", (socket) => {
    connectionCount += 1;
    const currentConnection = connectionCount;
    socket.once("message", (data, isBinary) => {
      assert.equal(isBinary, false);
      const auth = JSON.parse(data.toString("utf8"));
      authEpochs.push(auth.sourceEpoch);
      socket.send(JSON.stringify({
        type: "ready",
        protocol: 4,
        sourceEpoch: auth.sourceEpoch
      }));
      if (currentConnection === 1) {
        setTimeout(() => socket.terminate(), 20);
        return;
      }
      setTimeout(() => {
        socket.send(JSON.stringify({
          type: "keyframe",
          protocol: 4,
          reason: "loss_flush_or_session_start",
          requireCodecConfig: true
        }));
      }, 20);
      socket.once("message", (video, videoIsBinary) => {
        assert.equal(videoIsBinary, true);
        resolveRecoveredVideo(Uint8Array.from(video));
      });
    });
  });

  const connection = new DirectReceiverConnection({
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 11,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    heartbeatIntervalMs: 60_000
  });
  const closed = new Promise((resolve) => {
    connection.onClose = resolve;
  });
  const keyframeRequested = new Promise((resolve) => {
    connection.onControl = (message) => {
      if (message.type === "keyframe") {
        resolve(message);
      }
    };
  });

  await connection.connect();
  const closeEvent = await closed;
  assert.equal(closeEvent.code, 1006);
  const recovery = await connection.recover({
    timeoutMs: 2_000,
    connectTimeoutMs: 500,
    retryDelaysMs: [0, 10]
  });
  assert.equal(recovery.attempts, 1);
  assert.equal(connectionCount, 2);
  assert.deepEqual(authEpochs, [11, 11]);
  assert.equal((await keyframeRequested).requireCodecConfig, true);

  const annexB = Uint8Array.from([
    0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
    0, 0, 0, 1, 0x65, 0x04, 0x05, 0x06
  ]);
  const wireMessage = createDirectVideoMessage({
    byteLength: annexB.byteLength,
    timestamp: 88,
    type: "key",
    copyTo(destination) {
      destination.set(annexB);
    }
  }, 11, 9);
  assert.equal(connection.sendVideo(wireMessage), true);
  assert.deepEqual(await recoveredVideo, new Uint8Array(wireMessage));
  await connection.close();
});

test("direct WebSocket recovery stops immediately when Receiver rejects auth", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let attempts = 0;
  server.on("connection", (socket) => {
    socket.once("message", () => {
      socket.send(JSON.stringify({
        type: "error",
        protocol: 4,
        code: "identity_mismatch"
      }));
    });
  });

  const connection = new DirectReceiverConnection({
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 12,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    heartbeatIntervalMs: 60_000
  });
  const startedAt = Date.now();
  await assert.rejects(
    connection.recover({
      timeoutMs: 80,
      connectTimeoutMs: 40,
      retryDelaysMs: [0, 10],
      onAttempt() {
        attempts += 1;
      }
    }),
    /平板身份不匹配/
  );
  assert.equal(attempts, 1);
  assert.ok(Date.now() - startedAt < 500);
  assert.equal(connection.connected, false);
});

test("direct WebSocket recovery bounds repeated network handshake timeouts", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let attempts = 0;
  server.on("connection", (socket) => {
    socket.once("message", () => {
      // Keep the socket silent to model a Receiver that is not yet reachable
      // after the tablet wakes.
    });
  });

  const connection = new DirectReceiverConnection({
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 13,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    heartbeatIntervalMs: 60_000
  });
  const startedAt = Date.now();
  await assert.rejects(
    connection.recover({
      timeoutMs: 100,
      connectTimeoutMs: 20,
      retryDelaysMs: [0, 10],
      onAttempt() {
        attempts += 1;
      }
    }),
    /平板连接恢复超时/
  );
  assert.ok(attempts >= 2);
  assert.ok(Date.now() - startedAt < 500);
  assert.equal(connection.connected, false);
});

test("direct WebSocket recovery cancellation prevents an old source from retrying", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  server.on("connection", (socket) => {
    socket.once("message", () => {
      // The pending handshake is cancelled by a stop/new-source generation.
    });
  });

  const connection = new DirectReceiverConnection({
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 14,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    heartbeatIntervalMs: 60_000
  });
  let generation = 1;
  let attempts = 0;
  const recovery = connection.recover({
    timeoutMs: 1_000,
    connectTimeoutMs: 30,
    retryDelaysMs: [0, 10],
    shouldContinue: () => generation === 1,
    onAttempt() {
      attempts += 1;
      generation = 2;
    }
  });
  await assert.rejects(recovery, /平板连接恢复超时/);
  assert.equal(attempts, 1);
  assert.equal(connection.connected, false);
});
