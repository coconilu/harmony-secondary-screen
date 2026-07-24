import test from "node:test";
import assert from "node:assert/strict";

import {
  createLocalVideoMessage,
  LOCAL_VIDEO_HEADER_SIZE,
  LOCAL_VIDEO_MAGIC
} from "../local-relay-protocol.js";

test("serializes an H.264 chunk into the local Relay wire format", () => {
  const payload = Uint8Array.from([0, 0, 0, 1, 0x65]);
  const chunk = {
    byteLength: payload.byteLength,
    timestamp: 1_234_567,
    type: "key",
    copyTo(destination) {
      destination.set(payload);
    }
  };

  const message = createLocalVideoMessage(chunk, 42);
  const view = new DataView(message);
  assert.equal(view.getUint32(0, false), LOCAL_VIDEO_MAGIC);
  assert.equal(view.getUint8(4), 1);
  assert.equal(view.getUint8(5), 1);
  assert.equal(view.getUint16(6, false), LOCAL_VIDEO_HEADER_SIZE);
  assert.equal(view.getUint32(8, false), 42);
  assert.equal(view.getUint32(12, false), payload.byteLength);
  assert.equal(view.getBigUint64(16, false), 1_234_567n);
  assert.deepEqual(
    new Uint8Array(message, LOCAL_VIDEO_HEADER_SIZE),
    payload
  );
});

test("rejects invalid timestamps and oversized payloads", () => {
  const baseChunk = {
    byteLength: 1,
    timestamp: -1,
    type: "delta",
    copyTo() {}
  };
  assert.throws(
    () => createLocalVideoMessage(baseChunk, 0),
    /时间戳无效/
  );
  assert.throws(
    () =>
      createLocalVideoMessage(
        { ...baseChunk, timestamp: 0, byteLength: 8 * 1024 * 1024 + 1 },
        0
      ),
    /长度无效/
  );
});
