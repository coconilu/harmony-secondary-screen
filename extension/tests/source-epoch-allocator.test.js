import test from "node:test";
import assert from "node:assert/strict";

import { createSerializedSourceEpochAllocator } from "../source-epoch-allocator.js";

test("concurrent capture start and reconfiguration allocate distinct persistent epochs", async () => {
  const values = {};
  const storage = {
    async get(key) {
      await delay(2);
      return { [key]: values[key] };
    },
    async set(update) {
      await delay(2);
      Object.assign(values, update);
    }
  };
  const allocate = createSerializedSourceEpochAllocator(storage);
  assert.deepEqual(
    await Promise.all([allocate(), allocate(), allocate()]),
    [1, 2, 3]
  );
  assert.equal(values.sourceEpoch, 3);
});

test("a failed allocation does not poison the serialized queue", async () => {
  let calls = 0;
  const values = {};
  const storage = {
    async get(key) {
      calls += 1;
      if (calls === 1) {
        throw new Error("simulated storage interruption");
      }
      return { [key]: values[key] };
    },
    async set(update) {
      Object.assign(values, update);
    }
  };
  const allocate = createSerializedSourceEpochAllocator(storage);
  await assert.rejects(allocate(), /interruption/);
  assert.equal(await allocate(), 1);
});

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
