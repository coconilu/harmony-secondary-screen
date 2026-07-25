import test from "node:test";
import assert from "node:assert/strict";

import {
  acquireHostPermission,
  cleanupUnusedManualHostPermissions,
  withHostPermission
} from "../host-permissions.js";

function createPermissions(initialOrigins = [], removeResult = true) {
  const origins = new Set(initialOrigins);
  const calls = {
    requested: [],
    removed: []
  };
  return {
    calls,
    async contains({ origins: queried }) {
      return queried.every((origin) => origins.has(origin));
    },
    async request({ origins: requested }) {
      calls.requested.push(...requested);
      requested.forEach((origin) => origins.add(origin));
      return true;
    },
    async remove({ origins: removed }) {
      calls.removed.push(...removed);
      if (!removeResult) return false;
      removed.forEach((origin) => origins.delete(origin));
      return true;
    },
    async getAll() {
      return { origins: [...origins] };
    }
  };
}

test("failed pairing rolls back only the permission acquired for that attempt", async () => {
  const permissions = createPermissions([
    "http://192.168.1.8/*"
  ]);
  const existing = await acquireHostPermission("192.168.1.8", permissions);
  assert.deepEqual(existing, {
    origin: "http://192.168.1.8/*",
    added: false
  });

  await assert.rejects(
    withHostPermission(
      "192.168.1.9",
      async () => {
        throw new Error("pairing failed");
      },
      permissions
    ),
    /pairing failed/
  );
  assert.deepEqual(permissions.calls.requested, [
    "http://192.168.1.9/*"
  ]);
  assert.deepEqual(permissions.calls.removed, [
    "http://192.168.1.9/*"
  ]);
  assert.deepEqual((await permissions.getAll()).origins, [
    "http://192.168.1.8/*"
  ]);
});

test("cleanup enumerates grants and removes every unused manual private origin", async () => {
  const permissions = createPermissions([
    "http://harmony-web-companion.local/*",
    "http://192.168.1.8/*",
    "http://192.168.1.9/*",
    "http://10.0.0.2/*"
  ]);
  const removed = await cleanupUnusedManualHostPermissions(
    ["192.168.1.9"],
    permissions
  );
  assert.deepEqual(removed, [
    "http://192.168.1.8/*",
    "http://10.0.0.2/*"
  ]);
  assert.deepEqual((await permissions.getAll()).origins.sort(), [
    "http://192.168.1.9/*",
    "http://harmony-web-companion.local/*"
  ]);
});

test("a false permissions.remove result is surfaced instead of treated as cleanup", async () => {
  const permissions = createPermissions(
    ["http://192.168.1.8/*"],
    false
  );
  await assert.rejects(
    cleanupUnusedManualHostPermissions([], permissions),
    /无法撤销/
  );
});
