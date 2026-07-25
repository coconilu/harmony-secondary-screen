import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

import { createDirectVideoMessage } from "../../extension/direct-protocol.js";

const outputPath = process.argv[2];
if (!outputPath) {
  throw new Error("usage: node write_extension_wire_fixture.js <output>");
}

const annexB = Uint8Array.from([
  0, 0, 0, 1, 0x67, 0x42, 0, 0x1f,
  0, 0, 0, 1, 0x65, 0x88, 0x84
]);
const wireMessage = createDirectVideoMessage({
  byteLength: annexB.byteLength,
  timestamp: 1_234_567,
  type: "key",
  copyTo(destination) {
    destination.set(annexB);
  }
}, 9, 42);

const absolutePath = resolve(outputPath);
await mkdir(dirname(absolutePath), { recursive: true });
await writeFile(absolutePath, new Uint8Array(wireMessage));
