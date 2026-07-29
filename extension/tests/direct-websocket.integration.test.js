import test from "node:test";
import assert from "node:assert/strict";
import { once } from "node:events";
import { WebSocket, WebSocketServer } from "ws";

import {
  DIRECT_RECOVERY_MAX_RETRY_DELAY_MS,
  DirectReceiverConnection,
  pairReceiver,
  ReceiverRecoveryCancelledError
} from "../direct-client.js";
import {
  describeDirectTransportFailure,
  DirectRequestObserver
} from "../direct-network-diagnostics.js";
import { createDirectVideoMessage } from "../direct-protocol.js";

globalThis.WebSocket = WebSocket;

const TEST_TRANSPORT_OBSERVATION = Object.freeze({
  beginTransportObservation: async () => "explicit-test-observation",
  finishTransportObservation: async () => ({
    allowAuthentication: true,
    recoverable: true
  })
});

const EDGE_OBSERVATION_CONTEXT = Object.freeze({
  documentId: null,
  initiator: "chrome-extension://abcdefghijklmnopabcdefghijklmnop",
  page: "setup.html"
});

function createEdgeFallbackTransport(observedIp) {
  const observer = new DirectRequestObserver();
  const pending = [];
  let requestSequence = 0;
  return {
    beginTransportObservation: async (url) => {
      const attemptId = observer.begin(url, EDGE_OBSERVATION_CONTEXT);
      pending.push({ attemptId, url });
      return attemptId;
    },
    finishTransportObservation: async (attemptId, host, socketOutcome) =>
      describeDirectTransportFailure({
        host,
        ...observer.finish(
          attemptId,
          socketOutcome,
          EDGE_OBSERVATION_CONTEXT
        )
      }),
    wrapSocketFactory(actualUrl) {
      return () => {
        const attempt = pending.shift();
        assert.ok(attempt);
        requestSequence += 1;
        const details = {
          requestId: `edge-fallback-${requestSequence}`,
          url: attempt.url,
          type: "websocket",
          tabId: -1,
          frameId: 0,
          parentFrameId: -1
        };
        observer.observeBefore(details);
        observer.observeTerminal(observedIp === undefined
          ? details
          : { ...details, ip: observedIp });
        return new WebSocket(actualUrl);
      };
    }
  };
}

function isRedactedDiagnosticFailure(error) {
  assert.equal(error.message.includes("AD1"), false);
  assert.match(error.diagnosticCode, /^AD1\|B=1\|Q=bound/);
  for (const sensitive of [
    "203.0.113.8",
    "harmony-web-companion.local",
    "1".repeat(32),
    "2".repeat(64),
    "a".repeat(64)
  ]) {
    assert.equal(error.message.includes(sensitive), false);
    assert.equal(error.diagnosticCode.includes(sensitive), false);
  }
  return true;
}

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
    ...TEST_TRANSPORT_OBSERVATION,
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

test("expired QR is reported as authorization expiry, not a network failure", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  server.once("connection", (socket) => {
    socket.once("message", () => {
      socket.send(JSON.stringify({
        type: "error",
        protocol: 4,
        code: "authorization_expired"
      }));
    });
  });

  await assert.rejects(
    pairReceiver({
      ...TEST_TRANSPORT_OBSERVATION,
      host: "192.168.1.8",
      authorization: {
        sessionId: "1".repeat(32),
        token: "2".repeat(64)
      },
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`)
    }),
    /二维码或短码已经过期，请重新生成/
  );
});

test("Edge no-context fallback sends pairing only after a private IP verdict", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let receivedRequests = 0;
  server.once("connection", (socket) => {
    socket.once("message", (data) => {
      receivedRequests += 1;
      const pair = JSON.parse(data.toString("utf8"));
      assert.equal(pair.token, "2".repeat(64));
      socket.send(JSON.stringify({
        type: "paired",
        protocol: 4,
        deviceId: "00112233445566778899aabbccddeeff",
        credential: "a".repeat(64)
      }));
    });
  });
  const transport = createEdgeFallbackTransport("192.168.1.8");
  const trusted = await pairReceiver({
    ...transport,
    host: "harmony-web-companion.local",
    authorization: {
      sessionId: "1".repeat(32),
      token: "2".repeat(64)
    },
    senderId: "019fa3cf-75c7-7000-8000-000000000001",
    socketFactory: transport.wrapSocketFactory(
      `ws://127.0.0.1:${address.port}`
    )
  });
  assert.equal(receivedRequests, 1);
  assert.equal(trusted.credential, "a".repeat(64));
});

test("Edge fallback sends no token or credential without a private IP", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let receivedRequests = 0;
  server.on("connection", (socket) => {
    socket.on("message", () => {
      receivedRequests += 1;
    });
  });
  const actualUrl = `ws://127.0.0.1:${address.port}`;

  for (const observedIp of [undefined, "203.0.113.8"]) {
    const pairingTransport = createEdgeFallbackTransport(observedIp);
    await assert.rejects(
      pairReceiver({
        ...pairingTransport,
        host: "harmony-web-companion.local",
        authorization: {
          sessionId: "1".repeat(32),
          token: "2".repeat(64)
        },
        senderId: "019fa3cf-75c7-7000-8000-000000000001",
        socketFactory: pairingTransport.wrapSocketFactory(actualUrl)
      }),
      isRedactedDiagnosticFailure
    );

    const authTransport = createEdgeFallbackTransport(observedIp);
    const connection = new DirectReceiverConnection({
      ...authTransport,
      trustedDevice: {
        senderId: "019fa3cf-75c7-7000-8000-000000000001",
        deviceId: "00112233445566778899aabbccddeeff",
        credential: "a".repeat(64),
        host: "harmony-web-companion.local",
        pairedAt: 1
      },
      sourceEpoch: 9,
      socketFactory: authTransport.wrapSocketFactory(actualUrl),
      heartbeatIntervalMs: 60_000
    });
    await assert.rejects(
      connection.connect(),
      isRedactedDiagnosticFailure
    );
    assert.equal(connection.connected, false);
  }
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(receivedRequests, 0);
});

test("missing runtime observation sends neither pairing nor auth credentials", async (context) => {
  const previousChrome = globalThis.chrome;
  delete globalThis.chrome;
  context.after(() => {
    if (previousChrome === undefined) {
      delete globalThis.chrome;
    } else {
      globalThis.chrome = previousChrome;
    }
  });

  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let socketFactoryCalls = 0;
  let receivedRequests = 0;
  server.on("connection", (socket) => {
    socket.on("message", () => {
      receivedRequests += 1;
    });
  });
  const socketFactory = () => {
    socketFactoryCalls += 1;
    return new WebSocket(`ws://127.0.0.1:${address.port}`);
  };

  await assert.rejects(
    pairReceiver({
      host: "harmony-web-companion.local",
      authorization: {
        sessionId: "1".repeat(32),
        token: "2".repeat(64)
      },
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      socketFactory
    }),
    /安全检查不可用/
  );

  const connection = new DirectReceiverConnection({
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "harmony-web-companion.local",
      pairedAt: 1
    },
    sourceEpoch: 9,
    socketFactory,
    heartbeatIntervalMs: 60_000
  });
  await assert.rejects(connection.connect(), /安全检查不可用/);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(socketFactoryCalls, 0);
  assert.equal(receivedRequests, 0);
  assert.equal(connection.connected, false);
});

test("unsolicited paired response cannot bypass the private-address gate", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let receivedRequests = 0;
  server.once("connection", (socket) => {
    socket.on("message", () => {
      receivedRequests += 1;
    });
    socket.send(JSON.stringify({
      type: "paired",
      protocol: 4,
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64)
    }));
  });

  await assert.rejects(
    pairReceiver({
      ...TEST_TRANSPORT_OBSERVATION,
      host: "harmony-web-companion.local",
      authorization: {
        sessionId: "1".repeat(32),
        token: "2".repeat(64)
      },
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
      beginTransportObservation: async () => "delayed-gate",
      finishTransportObservation: async () => {
        await new Promise((resolve) => setTimeout(resolve, 50));
        return { allowAuthentication: true };
      }
    }),
    /请求发送前返回了响应/
  );
  await new Promise((resolve) => setTimeout(resolve, 60));
  assert.equal(receivedRequests, 0);
});

test("unsolicited ready response cannot authenticate or enable media", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  let receivedRequests = 0;
  server.once("connection", (socket) => {
    socket.on("message", () => {
      receivedRequests += 1;
    });
    socket.send(JSON.stringify({
      type: "ready",
      protocol: 4,
      sourceEpoch: 9
    }));
  });

  const connection = new DirectReceiverConnection({
    ...TEST_TRANSPORT_OBSERVATION,
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "harmony-web-companion.local",
      pairedAt: 1
    },
    sourceEpoch: 9,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    beginTransportObservation: async () => "delayed-gate",
    finishTransportObservation: async () => {
      await new Promise((resolve) => setTimeout(resolve, 50));
      return { allowAuthentication: true };
    }
  });
  await assert.rejects(connection.connect(), /请求发送前返回了响应/);
  await new Promise((resolve) => setTimeout(resolve, 60));
  assert.equal(connection.connected, false);
  assert.equal(receivedRequests, 0);
  assert.throws(
    () => connection.sendVideo(new ArrayBuffer(1)),
    /平板连接尚未建立/
  );
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
    ...TEST_TRANSPORT_OBSERVATION,
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

test("direct WebSocket does not expose a reconnect as connected before ready", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");

  let releaseReady;
  const readyGate = new Promise((resolve) => {
    releaseReady = resolve;
  });
  let resolveAuthReceived;
  const authReceived = new Promise((resolve) => {
    resolveAuthReceived = resolve;
  });
  server.once("connection", (socket) => {
    socket.once("message", async (data, isBinary) => {
      assert.equal(isBinary, false);
      const auth = JSON.parse(data.toString("utf8"));
      resolveAuthReceived();
      await readyGate;
      socket.send(JSON.stringify({
        type: "ready",
        protocol: 4,
        sourceEpoch: auth.sourceEpoch
      }));
    });
  });

  const connection = new DirectReceiverConnection({
    ...TEST_TRANSPORT_OBSERVATION,
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: ["192", "168", "1", "8"].join("."),
      pairedAt: 1
    },
    sourceEpoch: 8,
    socketFactory: () => new WebSocket(`ws://127.0.0.1:${address.port}`),
    heartbeatIntervalMs: 60_000
  });
  const connecting = connection.connect({ timeoutMs: 500 });
  await authReceived;
  assert.equal(connection.connected, false);
  assert.throws(
    () => connection.sendVideo(new ArrayBuffer(1)),
    /平板连接尚未建立/
  );
  releaseReady();
  await connecting;
  assert.equal(connection.connected, true);
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
    ...TEST_TRANSPORT_OBSERVATION,
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
    ...TEST_TRANSPORT_OBSERVATION,
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

test("direct WebSocket recovery survives more than 184 seconds without buffering video", async (context) => {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => server.close());
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  const authEpochs = [];
  const receivedVideo = [];
  let resolveKeyframe;
  const keyframeRequested = new Promise((resolve) => {
    resolveKeyframe = resolve;
  });
  let resolveVideo;
  const firstVideo = new Promise((resolve) => {
    resolveVideo = resolve;
  });
  server.on("connection", (socket) => {
    socket.once("message", (data, isBinary) => {
      assert.equal(isBinary, false);
      const auth = JSON.parse(data.toString("utf8"));
      authEpochs.push(auth.sourceEpoch);
      socket.send(JSON.stringify({
        type: "ready",
        protocol: 4,
        sourceEpoch: auth.sourceEpoch
      }));
      setTimeout(() => {
        socket.send(JSON.stringify({
          type: "keyframe",
          protocol: 4,
          reason: "loss_flush_or_session_start",
          requireCodecConfig: true
        }));
      }, 10);
      socket.on("message", (video, videoIsBinary) => {
        if (!videoIsBinary) {
          return;
        }
        const received = Uint8Array.from(video);
        receivedVideo.push(received);
        resolveVideo(received);
      });
    });
  });

  const connection = new DirectReceiverConnection({
    ...TEST_TRANSPORT_OBSERVATION,
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
  connection.onControl = (message) => {
    if (message.type === "keyframe") {
      resolveKeyframe(message);
    }
  };

  let virtualNowMs = 0;
  const attemptElapsedTimes = [];
  const actualRetryDelays = [];
  const disconnectedPayload = createDirectVideoMessage({
    byteLength: 4,
    timestamp: 50,
    type: "delta",
    copyTo(destination) {
      destination.set(Uint8Array.from([0, 0, 1, 0x41]));
    }
  }, 13, 1);
  let disconnectedPayloadDrops = 0;
  const recovery = await connection.recover({
    connectTimeoutMs: 1_500,
    retryDelaysMs: [0, 250, 500, 1_000, 5_000],
    now: () => virtualNowMs,
    delayFn: async (milliseconds, signal) => {
      assert.equal(signal?.aborted ?? false, false);
      actualRetryDelays.push(milliseconds);
      virtualNowMs += milliseconds;
    },
    onAttempt({ elapsedMs }) {
      attemptElapsedTimes.push(elapsedMs);
    },
    connectAttempt: async (options) => {
      if (virtualNowMs < 184_180) {
        assert.throws(
          () => connection.sendVideo(disconnectedPayload),
          /平板连接尚未建立/
        );
        disconnectedPayloadDrops += 1;
        virtualNowMs += options.timeoutMs;
        throw new Error("模拟平板休眠期间网络不可达");
      }
      return connection.connect(options);
    }
  });

  assert.ok(recovery.elapsedMs >= 184_180);
  assert.ok(attemptElapsedTimes.some((elapsedMs) => elapsedMs > 120_000));
  assert.ok(actualRetryDelays.length > 1);
  assert.ok(disconnectedPayloadDrops > 0);
  assert.ok(
    actualRetryDelays.every(
      (milliseconds) => milliseconds <= DIRECT_RECOVERY_MAX_RETRY_DELAY_MS
    )
  );
  assert.deepEqual(authEpochs, [13]);
  assert.equal(receivedVideo.length, 0);
  assert.equal((await keyframeRequested).requireCodecConfig, true);

  const annexB = Uint8Array.from([
    0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
    0, 0, 0, 1, 0x65, 0x07, 0x08, 0x09
  ]);
  const wireMessage = createDirectVideoMessage({
    byteLength: annexB.byteLength,
    timestamp: 99,
    type: "key",
    copyTo(destination) {
      destination.set(annexB);
    }
  }, 13, 10);
  assert.equal(connection.sendVideo(wireMessage), true);
  assert.deepEqual(await firstVideo, new Uint8Array(wireMessage));
  assert.equal(receivedVideo.length, 1);
  await connection.close();
});

test("direct WebSocket recovery cancellation prevents an old source from retrying", async () => {
  const connection = new DirectReceiverConnection({
    ...TEST_TRANSPORT_OBSERVATION,
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 14,
    socketFactory: () => {
      throw new Error("取消后不应再创建 WebSocket");
    },
    heartbeatIntervalMs: 60_000
  });
  const abortController = new AbortController();
  let attempts = 0;
  const recovery = connection.recover({
    connectTimeoutMs: 1_500,
    retryDelaysMs: [0, 10],
    signal: abortController.signal,
    connectAttempt: async () => {
      throw new Error("模拟瞬态网络错误");
    },
    onAttempt() {
      attempts += 1;
      abortController.abort();
    }
  });
  await assert.rejects(
    recovery,
    (error) => error instanceof ReceiverRecoveryCancelledError
  );
  assert.equal(attempts, 1);
  assert.equal(connection.connected, false);
});

test("direct WebSocket recovery generation change cannot overwrite a new source", async () => {
  const connection = new DirectReceiverConnection({
    ...TEST_TRANSPORT_OBSERVATION,
    trustedDevice: {
      senderId: "019fa3cf-75c7-7000-8000-000000000001",
      deviceId: "00112233445566778899aabbccddeeff",
      credential: "a".repeat(64),
      host: "192.168.1.8",
      pairedAt: 1
    },
    sourceEpoch: 15,
    socketFactory: () => {
      throw new Error("旧来源取消后不应创建 WebSocket");
    },
    heartbeatIntervalMs: 60_000
  });
  let generation = 1;
  let attempts = 0;
  const recovery = connection.recover({
    retryDelaysMs: [0, 10],
    shouldContinue: () => generation === 1,
    connectAttempt: async () => {
      throw new Error("模拟旧来源瞬态网络错误");
    },
    onAttempt() {
      attempts += 1;
      generation = 2;
    }
  });
  await assert.rejects(
    recovery,
    (error) => error instanceof ReceiverRecoveryCancelledError
  );
  assert.equal(attempts, 1);
  assert.equal(connection.connected, false);
});
