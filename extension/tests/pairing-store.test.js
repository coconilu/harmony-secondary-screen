import test from "node:test";
import assert from "node:assert/strict";

import {
  allocateSourceEpoch,
  forgetTrustedReceiver,
  getTrustedReceiver,
  saveTrustedReceiver
} from "../pairing-store.js";

function createStorage() {
  const values = new Map();
  return {
    async get(key) {
      return { [key]: values.get(key) };
    },
    async set(entries) {
      for (const [key, value] of Object.entries(entries)) values.set(key, value);
    },
    async remove(key) {
      values.delete(key);
    }
  };
}

test("trusted identity survives address changes until explicitly forgotten", async () => {
  const storage = createStorage();
  const original = {
    senderId: "019fa3cf-75c7-7000-8000-000000000001",
    deviceId: "00112233445566778899aabbccddeeff",
    credential: "a".repeat(64),
    host: "192.168.1.8",
    pairedAt: 123
  };
  await saveTrustedReceiver(original, storage);
  assert.deepEqual(await getTrustedReceiver(storage), original);

  await saveTrustedReceiver({ ...original, host: "192.168.1.99" }, storage);
  const moved = await getTrustedReceiver(storage);
  assert.equal(moved.deviceId, original.deviceId);
  assert.equal(moved.credential, original.credential);
  assert.equal(moved.host, "192.168.1.99");

  await forgetTrustedReceiver(storage);
  assert.equal(await getTrustedReceiver(storage), null);
});

test("an HWC5-era credential record migrates to HWC6 without rescanning", async () => {
  const storage = createStorage();
  const legacyRecord = {
    senderId: "019fa3cf-75c7-7000-8000-000000000001",
    deviceId: "00112233445566778899aabbccddeeff",
    credential: "b".repeat(64),
    host: "harmony-web-companion.local",
    pairedAt: 456
  };
  await storage.set({ trustedReceiver: legacyRecord });
  assert.deepEqual(await getTrustedReceiver(storage), legacyRecord);
});

test("source epoch increases monotonically across capture starts", async () => {
  const storage = createStorage();
  assert.equal(await allocateSourceEpoch(storage), 1);
  assert.equal(await allocateSourceEpoch(storage), 2);
  assert.equal(await allocateSourceEpoch(storage), 3);
});
