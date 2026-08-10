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
  computeAuthProof,
  computePairProof,
  createDirectVideoMessage,
  DIRECT_PROTOCOL
} from "../direct-protocol.js";

globalThis.WebSocket = WebSocket;

const TOKEN = "2".repeat(64);
const CREDENTIAL = "a".repeat(64);
const DEVICE_ID = "00112233445566778899aabbccddeeff";
const SESSION_ID = "1".repeat(32);
const SENDER_ID = "019fa3cf-75c7-7000-8000-000000000001";
const MEDIA_CONTRACT = Object.freeze({
  width: 1920,
  height: 1080,
  maxFps: 60
});
const TRUSTED = Object.freeze({
  senderId: SENDER_ID,
  deviceId: DEVICE_ID,
  credential: CREDENTIAL,
  host: "192.168.1.8",
  pairedAt: 1
});

test("valid QR proof releases the token and completes pairing", async (context) => {
  const { server, url } = await createServer(context);
  const received = [];
  const completed = new Promise((resolve, reject) => {
    server.once("connection", (socket) => {
      socket.on("message", async (data, isBinary) => {
        try {
          assert.equal(isBinary, false);
          const message = JSON.parse(data.toString("utf8"));
          received.push(message);
          if (message.type === "pair_challenge") {
            assert.equal("token" in message, false);
            socket.send(JSON.stringify(await pairProof(message)));
            return;
          }
          assert.equal(message.type, "pair");
          assert.equal(message.token, TOKEN);
          socket.send(JSON.stringify({
            type: "paired",
            protocol: DIRECT_PROTOCOL,
            deviceId: DEVICE_ID,
            credential: CREDENTIAL
          }));
          resolve();
        } catch (error) {
          reject(error);
        }
      });
    });
  });
  const trusted = await pairReceiver({
    host: "tabreach.local",
    authorization: { sessionId: SESSION_ID, token: TOKEN },
    senderId: SENDER_ID,
    socketFactory: () => new WebSocket(url)
  });
  await completed;
  assert.equal(trusted.credential, CREDENTIAL);
  assert.equal(received.length, 2);
  assert.equal(JSON.stringify(received[0]).includes(TOKEN), false);
});

test("manual private IPv4 preserves short-code pairing without a low-entropy proof", async (context) => {
  const { server, url } = await createServer(context);
  server.once("connection", (socket) => {
    socket.once("message", (data) => {
      const pair = JSON.parse(data.toString("utf8"));
      assert.equal(pair.type, "pair_manual_ipv4");
      assert.equal(pair.mode, "pair_manual_ipv4");
      assert.equal(pair.token, TOKEN);
      assert.equal("nonce" in pair, false);
      socket.send(JSON.stringify({
        type: "paired",
        protocol: DIRECT_PROTOCOL,
        deviceId: DEVICE_ID,
        credential: CREDENTIAL
      }));
    });
  });
  const trusted = await pairReceiver({
    host: "192.168.1.8",
    authorization: { sessionId: SESSION_ID, token: TOKEN },
    senderId: SENDER_ID,
    socketFactory: () => new WebSocket(url)
  });
  assert.equal(trusted.deviceId, DEVICE_ID);
});

test("automatic address refuses short-code proof mode and releases zero token", async (context) => {
  const { server, url } = await createServer(context);
  const received = [];
  server.once("connection", (socket) => {
    socket.on("message", (data) => {
      received.push(JSON.parse(data.toString("utf8")));
      socket.send(JSON.stringify({
        type: "error",
        protocol: DIRECT_PROTOCOL,
        code: "short_code_requires_manual_ipv4"
      }));
    });
  });
  await assert.rejects(pairReceiver({
    host: "tabreach.local",
    authorization: { sessionId: SESSION_ID, token: TOKEN },
    senderId: SENDER_ID,
    socketFactory: () => new WebSocket(url)
  }), /数字私网 IPv4/);
  await delay(10);
  assert.equal(received.length, 1);
  assert.equal(JSON.stringify(received).includes(TOKEN), false);
});

test("oversized pair proof is rejected before JSON parsing or HMAC and releases zero token", async (context) => {
  const { server, url } = await createServer(context);
  const received = [];
  server.once("connection", (socket) => {
    socket.on("message", async (data) => {
      const message = JSON.parse(data.toString("utf8"));
      received.push(message);
      if (received.length === 1) {
        socket.send(JSON.stringify({
          ...(await pairProof(message)),
          padding: "x".repeat(2_000_000)
        }));
      }
    });
  });
  await assert.rejects(pairReceiver({
    host: "tabreach.local",
    authorization: { sessionId: SESSION_ID, token: TOKEN },
    senderId: SENDER_ID,
    socketFactory: () => new WebSocket(url)
  }), /超过 2048 字节/);
  await delay(10);
  assert.equal(received.length, 1);
  assert.equal(JSON.stringify(received).includes(TOKEN), false);
});

test("expired authorization is distinct and releases no token", async (context) => {
  const { server, url } = await createServer(context);
  const received = [];
  server.once("connection", (socket) => {
    socket.on("message", (data) => received.push(JSON.parse(data.toString("utf8"))));
    socket.once("message", () => socket.send(JSON.stringify({
      type: "error",
      protocol: DIRECT_PROTOCOL,
      code: "authorization_expired"
    })));
  });
  await assert.rejects(pairReceiver({
    host: "tabreach.local",
    authorization: { sessionId: SESSION_ID, token: TOKEN },
    senderId: SENDER_ID,
    socketFactory: () => new WebSocket(url)
  }), /二维码或短码已经过期/);
  await delay(10);
  assert.equal(received.length, 1);
  assert.equal(JSON.stringify(received).includes(TOKEN), false);
});

test("forged, replayed, mismatched and premature pair proofs release zero token", async (context) => {
  const cases = [
    ["forged", (challenge) => ({
      ...basePairProof(challenge),
      proof: "0".repeat(64)
    })],
    ["replayed_nonce", async (challenge) => ({
      ...(await pairProof(challenge)),
      nonce: "0".repeat(64)
    })],
    ["wrong_mode", async (challenge) => ({
      ...(await pairProof(challenge)),
      mode: "auth"
    })],
    ["wrong_device", async (challenge) => ({
      ...(await pairProof(challenge)),
      deviceId: "f".repeat(32)
    })],
    ["wrong_sender", async (challenge) => ({
      ...(await pairProof(challenge)),
      senderId: "f".repeat(32)
    })],
    ["premature_paired", () => ({
      type: "paired",
      protocol: DIRECT_PROTOCOL,
      deviceId: DEVICE_ID,
      credential: CREDENTIAL
    })]
  ];
  for (const [name, responseFor] of cases) {
    const { server, url } = await createServer(context);
    const received = [];
    server.once("connection", (socket) => {
      socket.on("message", async (data) => {
        const message = JSON.parse(data.toString("utf8"));
        received.push(message);
        if (received.length === 1) {
          socket.send(JSON.stringify(await responseFor(message)));
        }
      });
    });
    await assert.rejects(pairReceiver({
      host: "tabreach.local",
      authorization: { sessionId: SESSION_ID, token: TOKEN },
      senderId: SENDER_ID,
      socketFactory: () => new WebSocket(url)
    }), /挑战证明|无效/);
    await delay(10);
    assert.equal(received.length, 1, name);
    assert.equal(JSON.stringify(received).includes(TOKEN), false, name);
    await closeServer(server);
  }
});

test("forged, replayed, mismatched and premature auth proofs release zero credential", async (context) => {
  const cases = [
    ["forged", (challenge) => ({
      ...baseAuthProof(challenge),
      proof: "0".repeat(64)
    })],
    ["replayed_nonce", async (challenge) => ({
      ...(await authProof(challenge)),
      nonce: "0".repeat(64)
    })],
    ["wrong_mode", async (challenge) => ({
      ...(await authProof(challenge)),
      mode: "pair"
    })],
    ["wrong_identity", async (challenge) => ({
      ...(await authProof(challenge)),
      deviceId: "f".repeat(32)
    })],
    ["premature_ready", (challenge) => ({
      type: "ready",
      protocol: DIRECT_PROTOCOL,
      sourceEpoch: challenge.sourceEpoch
    })]
  ];
  for (const [name, responseFor] of cases) {
    const { server, url } = await createServer(context);
    const received = [];
    server.once("connection", (socket) => {
      socket.on("message", async (data) => {
        const message = JSON.parse(data.toString("utf8"));
        received.push(message);
        if (received.length === 1) {
          socket.send(JSON.stringify(await responseFor(message)));
        }
      });
    });
    const connection = createConnection(url, 9);
    await assert.rejects(connection.connect(), /挑战证明|无效/);
    await delay(10);
    assert.equal(received.length, 1, name);
    assert.equal(JSON.stringify(received).includes(CREDENTIAL), false, name);
    assert.equal(connection.connected, false);
    await closeServer(server);
  }
});

test("oversized auth proof is rejected before JSON parsing or HMAC and releases zero credential", async (context) => {
  const { server, url } = await createServer(context);
  const received = [];
  server.once("connection", (socket) => {
    socket.on("message", async (data) => {
      const message = JSON.parse(data.toString("utf8"));
      received.push(message);
      if (received.length === 1) {
        socket.send(JSON.stringify({
          ...(await authProof(message)),
          padding: "界".repeat(700)
        }));
      }
    });
  });
  const connection = createConnection(url, 9);
  await assert.rejects(connection.connect(), /超过 2048 字节/);
  await delay(10);
  assert.equal(received.length, 1);
  assert.equal(JSON.stringify(received).includes(CREDENTIAL), false);
  assert.equal(connection.connected, false);
});

test("valid auth proof releases credential and permits one Annex-B AU", async (context) => {
  const { server, url } = await createServer(context);
  const receivedMessages = [];
  let resolveVideo;
  const receivedVideo = new Promise((resolve) => {
    resolveVideo = resolve;
  });
  server.once("connection", (socket) => {
    socket.on("message", async (data, isBinary) => {
      if (isBinary) {
        resolveVideo(Uint8Array.from(data));
        return;
      }
      const message = JSON.parse(data.toString("utf8"));
      receivedMessages.push(message);
      if (message.type === "auth_challenge") {
        assert.equal("credential" in message, false);
        socket.send(JSON.stringify(await authProof(message)));
      } else if (message.type === "auth") {
        assert.equal(message.credential, CREDENTIAL);
        socket.send(JSON.stringify({
          type: "ready",
          protocol: DIRECT_PROTOCOL,
          sourceEpoch: message.sourceEpoch,
          width: message.width,
          height: message.height,
          maxFps: message.maxFps
        }));
      }
    });
  });
  const connection = createConnection(url, 7);
  await connection.connect();
  const wire = videoMessage(7, 3);
  assert.equal(connection.sendVideo(wire), true);
  assert.deepEqual(await receivedVideo, new Uint8Array(wire));
  assert.equal(JSON.stringify(receivedMessages[0]).includes(CREDENTIAL), false);
  await connection.close();
});

test("connection failure before proof has a network category and releases no secret", async () => {
  const connection = createConnection("ws://127.0.0.1:1", 8);
  await assert.rejects(connection.connect(), (error) => {
    assert.equal(error.code, "receiver_connection_failed");
    assert.match(error.message, /无法连接 Receiver/);
    return true;
  });
  assert.equal(connection.connected, false);
  await assert.rejects(pairReceiver({
    host: "tabreach.local",
    authorization: { sessionId: SESSION_ID, token: TOKEN },
    senderId: SENDER_ID,
    socketFactory: () => new WebSocket("ws://127.0.0.1:1")
  }), (error) => {
    assert.equal(error.code, "automatic_address_or_connection_failed");
    assert.match(error.message, /自动地址解析或 Receiver 连接失败/);
    return true;
  });
});

test("an HWC5 Receiver is rejected before the saved credential is released", async (context) => {
  const { server, url } = await createServer(context);
  const received = [];
  server.once("connection", (socket) => {
    socket.once("message", (data) => {
      received.push(JSON.parse(data.toString("utf8")));
      socket.send(JSON.stringify({
        type: "error",
        protocol: 5,
        code: "protocol_mismatch"
      }));
    });
  });
  const connection = createConnection(url, 9);
  await assert.rejects(connection.connect(), /同时更新/);
  assert.equal(received.length, 1);
  assert.equal(JSON.stringify(received).includes(CREDENTIAL), false);
});

test("ready must echo the exact HMAC-bound dynamic media contract", async (context) => {
  const { server, url } = await createServer(context);
  server.once("connection", (socket) => {
    socket.on("message", async (data) => {
      const message = JSON.parse(data.toString("utf8"));
      if (message.type === "auth_challenge") {
        socket.send(JSON.stringify(await authProof(message)));
      } else if (message.type === "auth") {
        socket.send(JSON.stringify({
          type: "ready",
          protocol: DIRECT_PROTOCOL,
          sourceEpoch: message.sourceEpoch,
          width: 1280,
          height: 720,
          maxFps: message.maxFps
        }));
      }
    });
  });
  const connection = createConnection(url, 10);
  await assert.rejects(connection.connect(), /挑战证明|无效/);
  assert.equal(connection.connected, false);
});

test("reconnect repeats the proof gate and keeps the same source epoch", async (context) => {
  const { server, url } = await createServer(context);
  const epochs = [];
  let count = 0;
  let resolveSecondReady;
  const secondReady = new Promise((resolve) => {
    resolveSecondReady = resolve;
  });
  server.on("connection", (socket) => {
    count += 1;
    const current = count;
    socket.on("message", async (data) => {
      const message = JSON.parse(data.toString("utf8"));
      if (message.type === "auth_challenge") {
        assert.equal("credential" in message, false);
        socket.send(JSON.stringify(await authProof(message)));
      } else if (message.type === "auth") {
        epochs.push(message.sourceEpoch);
        socket.send(JSON.stringify({
          type: "ready",
          protocol: DIRECT_PROTOCOL,
          sourceEpoch: message.sourceEpoch,
          width: message.width,
          height: message.height,
          maxFps: message.maxFps
        }));
        if (current === 1) {
          setTimeout(() => socket.terminate(), 10);
        } else {
          resolveSecondReady();
        }
      }
    });
  });
  const connection = createConnection(url, 11);
  const closed = new Promise((resolve) => {
    connection.onClose = resolve;
  });
  await connection.connect();
  await closed;
  const recovery = await connection.recover({
    connectTimeoutMs: 500,
    retryDelaysMs: [0, 10]
  });
  await secondReady;
  assert.equal(recovery.attempts, 1);
  assert.deepEqual(epochs, [11, 11]);
  await connection.close();
});

test("permanent identity rejection stops recovery after one attempt", async (context) => {
  const { server, url } = await createServer(context);
  server.on("connection", (socket) => {
    socket.once("message", () => socket.send(JSON.stringify({
      type: "error",
      protocol: DIRECT_PROTOCOL,
      code: "identity_mismatch"
    })));
  });
  const connection = createConnection(url, 12);
  let attempts = 0;
  await assert.rejects(connection.recover({
    connectTimeoutMs: 100,
    retryDelaysMs: [0, 10],
    onAttempt() {
      attempts += 1;
    }
  }), /平板身份不匹配/);
  assert.equal(attempts, 1);
});

test("recovery survives more than 184 seconds without buffering video", async (context) => {
  const { server, url } = await createServer(context);
  const epochs = [];
  server.on("connection", (socket) => {
    socket.on("message", async (data) => {
      const message = JSON.parse(data.toString("utf8"));
      if (message.type === "auth_challenge") {
        socket.send(JSON.stringify(await authProof(message)));
      } else if (message.type === "auth") {
        epochs.push(message.sourceEpoch);
        socket.send(JSON.stringify({
          type: "ready",
          protocol: DIRECT_PROTOCOL,
          sourceEpoch: message.sourceEpoch,
          width: message.width,
          height: message.height,
          maxFps: message.maxFps
        }));
      }
    });
  });
  const connection = createConnection(url, 13);
  let virtualNowMs = 0;
  const retryDelays = [];
  const disconnectedPayload = videoMessage(13, 1);
  const recovery = await connection.recover({
    connectTimeoutMs: 1_500,
    retryDelaysMs: [0, 250, 500, 1_000, 5_000],
    now: () => virtualNowMs,
    delayFn: async (milliseconds) => {
      retryDelays.push(milliseconds);
      virtualNowMs += milliseconds;
    },
    connectAttempt: async (options) => {
      if (virtualNowMs < 184_180) {
        assert.throws(
          () => connection.sendVideo(disconnectedPayload),
          /平板连接尚未建立/
        );
        virtualNowMs += options.timeoutMs;
        throw new Error("模拟平板休眠期间网络不可达");
      }
      return connection.connect(options);
    }
  });
  assert.ok(recovery.elapsedMs >= 184_180);
  assert.ok(retryDelays.length > 1);
  assert.ok(retryDelays.every(
    (value) => value <= DIRECT_RECOVERY_MAX_RETRY_DELAY_MS
  ));
  assert.deepEqual(epochs, [13]);
  await connection.close();
});

test("recovery cancellation and generation changes cannot revive old sources", async () => {
  for (const mode of ["abort", "generation"]) {
    const connection = createConnection("ws://127.0.0.1:1", 14);
    const abortController = new AbortController();
    let generation = 1;
    let attempts = 0;
    const recovery = connection.recover({
      retryDelaysMs: [0, 10],
      signal: abortController.signal,
      shouldContinue: () => generation === 1,
      connectAttempt: async () => {
        throw new Error("模拟瞬态网络错误");
      },
      onAttempt() {
        attempts += 1;
        if (mode === "abort") abortController.abort();
        if (mode === "generation") generation = 2;
      }
    });
    await assert.rejects(
      recovery,
      (error) => error instanceof ReceiverRecoveryCancelledError
    );
    assert.equal(attempts, 1);
    assert.equal(connection.connected, false);
  }
});

async function createServer(context) {
  const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
  context.after(() => closeServer(server));
  await once(server, "listening");
  const address = server.address();
  assert.equal(typeof address, "object");
  return { server, url: `ws://127.0.0.1:${address.port}` };
}

function closeServer(server) {
  if (!server || server._state === 2) return Promise.resolve();
  for (const client of server.clients ?? []) client.terminate();
  return new Promise((resolve) => server.close(resolve));
}

function createConnection(url, sourceEpoch) {
  return new DirectReceiverConnection({
    trustedDevice: TRUSTED,
    sourceEpoch,
    mediaContract: MEDIA_CONTRACT,
    socketFactory: () => new WebSocket(url),
    heartbeatIntervalMs: 60_000
  });
}

function basePairProof(challenge, proofMode = "qr") {
  return {
    type: "pair_proof",
    protocol: DIRECT_PROTOCOL,
    mode: "pair",
    proofMode,
    sessionId: challenge.sessionId,
    senderId: challenge.senderId,
    nonce: challenge.nonce,
    deviceId: DEVICE_ID
  };
}

async function pairProof(challenge, proofMode = "qr") {
  return {
    ...basePairProof(challenge, proofMode),
    proof: await computePairProof({
      token: TOKEN,
      proofMode,
      sessionId: challenge.sessionId,
      senderId: challenge.senderId,
      nonce: challenge.nonce,
      deviceId: DEVICE_ID
    })
  };
}

function baseAuthProof(challenge) {
  return {
    type: "auth_proof",
    protocol: DIRECT_PROTOCOL,
    mode: "auth",
    senderId: challenge.senderId,
    deviceId: challenge.deviceId,
    sourceEpoch: challenge.sourceEpoch,
    nonce: challenge.nonce
  };
}

async function authProof(challenge) {
  return {
    ...baseAuthProof(challenge),
    proof: await computeAuthProof({
      credential: CREDENTIAL,
      senderId: challenge.senderId,
      deviceId: challenge.deviceId,
      sourceEpoch: challenge.sourceEpoch,
      nonce: challenge.nonce,
      codec: challenge.codec,
      avcFormat: challenge.avcFormat,
      width: challenge.width,
      height: challenge.height,
      maxFps: challenge.maxFps
    })
  };
}

function videoMessage(sourceEpoch, sequence) {
  const annexB = Uint8Array.from([
    0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
    0, 0, 0, 1, 0x65, 1, 2, 3
  ]);
  return createDirectVideoMessage({
    byteLength: annexB.byteLength,
    timestamp: 55,
    type: "key",
    copyTo(destination) {
      destination.set(annexB);
    }
  }, sourceEpoch, sequence);
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
