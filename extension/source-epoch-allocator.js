import { allocateSourceEpoch } from "./pairing-store.js";

export function createSerializedSourceEpochAllocator(storage) {
  let allocationQueue = Promise.resolve();
  return function allocate() {
    const allocation = allocationQueue.then(() =>
      allocateSourceEpoch(storage)
    );
    allocationQueue = allocation.catch(() => {});
    return allocation;
  };
}
