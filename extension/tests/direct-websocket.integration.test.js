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
