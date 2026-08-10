import test from "node:test";
import assert from "node:assert/strict";

import { createPairingAuthorization } from "../direct-protocol.js";
import {
  getPendingPairing,
  PENDING_PAIRING_STORAGE_KEY,
  savePendingPairing
} from "../pending-pairing-store.js";

function createStorage() {
  const values = new Map();
  return {
    values,
    async get(key) {
      return { [key]: values.get(key) };
    },
    async set(entries) {
      for (const [key, value] of Object.entries(entries)) {
        values.set(key, value);
      }
    },
    async remove(key) {
      values.delete(key);
    }
  };
}

function createStoredAuthorization(now = 1_000) {
  const random = Uint8Array.from({ length: 48 }, (_, index) => index);
  let offset = 0;
  const authorization = createPairingAuthorization(now, (target) => {
    target.set(random.subarray(offset, offset + target.length));
    offset += target.length;
    return target;
  });
  return {
    sessionId: authorization.sessionId,
    token: authorization.token,
    shortCode: authorization.shortCode,
    expiresAt: authorization.expiresAt
  };
}

test("saving a pending legacy default host stores the new default", async () => {
  const storage = createStorage();
  const authorization = createStoredAuthorization();

  await savePendingPairing({
    host: "harmony-web-companion.local",
    authorization
  }, storage, 1_000);

  assert.equal(
    storage.values.get("pendingPairing").host,
    "tabreach.local"
  );
});

test("reading a raw legacy pending record migrates only its host", async () => {
  const storage = createStorage();
  const authorization = createStoredAuthorization();
  const stored = {
    host: "harmony-web-companion.local",
    authorization
  };
  storage.values.set(PENDING_PAIRING_STORAGE_KEY, stored);

  const pending = await getPendingPairing(storage, 1_000);

  assert.equal(pending.host, "tabreach.local");
  assert.equal(
    storage.values.get(PENDING_PAIRING_STORAGE_KEY).host,
    "tabreach.local"
  );
  assert.deepEqual(
    storage.values.get(PENDING_PAIRING_STORAGE_KEY).authorization,
    authorization
  );
});

test("a pending migration write failure stays observable without deleting authorization", async () => {
  const authorization = createStoredAuthorization();
  const stored = {
    host: "harmony-web-companion.local",
    authorization
  };
  let removed = false;
  const storage = {
    async get(key) {
      return { [key]: stored };
    },
    async set() {
      throw new Error("storage write failed");
    },
    async remove() {
      removed = true;
    }
  };

  await assert.rejects(
    getPendingPairing(storage, 1_000),
    /storage write failed/
  );
  assert.equal(removed, false);
  assert.equal(stored.host, "harmony-web-companion.local");
  assert.deepEqual(stored.authorization, authorization);
});
