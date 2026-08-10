import test from "node:test";
import assert from "node:assert/strict";

import { createPairingAuthorization } from "../direct-protocol.js";
import {
  getPendingPairing,
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

test("a pending legacy default host migrates inside session storage", async () => {
  const storage = createStorage();
  const random = Uint8Array.from({ length: 48 }, (_, index) => index);
  let offset = 0;
  const authorization = createPairingAuthorization(1_000, (target) => {
    target.set(random.subarray(offset, offset + target.length));
    offset += target.length;
    return target;
  });

  await savePendingPairing({
    host: "harmony-web-companion.local",
    authorization
  }, storage, 1_000);

  assert.equal(
    storage.values.get("pendingPairing").host,
    "tabreach.local"
  );
  assert.equal(
    (await getPendingPairing(storage, 1_000)).host,
    "tabreach.local"
  );
});
