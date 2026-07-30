import test from "node:test";
import assert from "node:assert/strict";

import {
  computeAuthProof,
  computePairProof,
  constantTimeEqualProof,
  createDirectVideoMessage,
  createPairingAuthorization,
  DIRECT_PROTOCOL,
  DIRECT_VIDEO_HEADER_SIZE,
  DIRECT_VIDEO_MAGIC,
  isLatestEpoch,
  normalizeReceiverHost,
  parsePairingAuthorization
} from "../direct-protocol.js";

test("serializes an Annex-B access unit byte-for-byte for the direct Receiver", () => {
  assert.equal(DIRECT_PROTOCOL, 5);
  assert.equal(DIRECT_VIDEO_MAGIC, 0x48574335);
  const annexB = Uint8Array.from([
    0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
    0, 0, 0, 1, 0x65, 0x88, 0x84
  ]);
  const chunk = {
    byteLength: annexB.byteLength,
    timestamp: 1_234_567,
    type: "key",
    copyTo(destination) {
      destination.set(annexB);
    }
  };
  const receivedByFakeReceiver = createDirectVideoMessage(chunk, 9, 42);
  const view = new DataView(receivedByFakeReceiver);
  assert.equal(view.getUint32(0, false), DIRECT_VIDEO_MAGIC);
  assert.equal(view.getUint8(4), DIRECT_PROTOCOL);
  assert.equal(view.getUint8(5), 1);
  assert.equal(view.getUint16(6, false), DIRECT_VIDEO_HEADER_SIZE);
  assert.equal(view.getUint32(8, false), 9);
  assert.equal(view.getUint32(12, false), 42);
  assert.equal(view.getUint32(16, false), annexB.byteLength);
  assert.equal(view.getUint32(20, false), 0);
  assert.equal(view.getBigUint64(24, false), 1_234_567n);
  assert.deepEqual(
    new Uint8Array(receivedByFakeReceiver, DIRECT_VIDEO_HEADER_SIZE),
    annexB
  );
});

test("source epoch accepts only the latest source and rejects delayed old frames", () => {
  assert.equal(isLatestEpoch(12, 12), true);
  assert.equal(isLatestEpoch(13, 12), true);
  assert.equal(isLatestEpoch(11, 12), false);
});

test("pairing authorization is valid once within sixty seconds", () => {
  const values = Uint8Array.from({ length: 48 }, (_, index) => index);
  let offset = 0;
  const authorization = createPairingAuthorization(1_000, (target) => {
    target.set(values.subarray(offset, offset + target.length));
    offset += target.length;
    return target;
  });
  assert.equal(authorization.expiresAt, 61_000);
  assert.equal(JSON.parse(authorization.payload).v, 5);
  assert.deepEqual(parsePairingAuthorization(authorization.payload, 60_999), {
    sessionId: authorization.sessionId,
    token: authorization.token,
    expiresAt: authorization.expiresAt,
    shortCode: authorization.shortCode
  });
  assert.throws(
    () => parsePairingAuthorization(authorization.payload, 61_000),
    /过期/
  );
  assert.equal(JSON.parse(authorization.payload).url, undefined);
});

test("HWC5 proof vectors match the native Receiver contract", async () => {
  const vector = {
    token: "00".repeat(32),
    credential: "11".repeat(32),
    sessionId: "00112233445566778899aabbccddeeff",
    nonce: "0123456789abcdef".repeat(4),
    deviceId: "ffeeddccbbaa99887766554433221100",
    senderId: "019fa3cf-75c7-7000-8000-000000000001"
  };
  assert.equal(await computePairProof({
    ...vector,
    proofMode: "qr"
  }), "e4da9d38094ddbee44ba4c26bac44afb562fdd22df65f25245a103441475bdc5");
  await assert.rejects(computePairProof({
    ...vector,
    proofMode: "short"
  }), /配对挑战字段无效/);
  assert.equal(await computeAuthProof({
    ...vector,
    sourceEpoch: 9
  }), "7453800320a532c293b9678cf1641fd503c92d005f8fe63e136e5d367e50997a");
  assert.equal(constantTimeEqualProof("a".repeat(64), "a".repeat(64)), true);
  assert.equal(constantTimeEqualProof("a".repeat(64), "b".repeat(64)), false);
  assert.equal(constantTimeEqualProof("a".repeat(63), "a".repeat(63)), false);
});

test("manual address accepts only private or link-local IPv4", () => {
  assert.equal(normalizeReceiverHost("192.168.1.8"), "192.168.1.8");
  assert.equal(normalizeReceiverHost("172.20.0.3"), "172.20.0.3");
  assert.equal(
    normalizeReceiverHost("harmony-web-companion.local"),
    "harmony-web-companion.local"
  );
  for (const value of ["0.0.0.0", "127.0.0.1", "8.8.8.8", "224.0.0.1"]) {
    assert.throws(() => normalizeReceiverHost(value));
  }
});
