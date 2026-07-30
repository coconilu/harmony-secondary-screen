import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = await readFile(
  new URL("../offscreen.js", import.meta.url),
  "utf8"
);

test("startup closes the sizing frame and reader before authentication", () => {
  const firstRead = source.indexOf("const first = await frameReader.read()");
  const frameClose = source.indexOf("first.value.close()", firstRead);
  const readerCancel = source.indexOf("await frameReader.cancel()", frameClose);
  const configure = source.indexOf(
    "await createVideoEncoder(firstWidth, firstHeight)",
    readerCancel
  );
  const authenticate = source.indexOf("await connectReceiver({", configure);
  const activeProcessor = source.indexOf(
    "const activeProcessor = new MediaStreamTrackProcessor",
    authenticate
  );
  assert.ok(firstRead >= 0);
  assert.ok(firstRead < frameClose);
  assert.ok(frameClose < readerCancel);
  assert.ok(readerCancel < configure);
  assert.ok(configure < authenticate);
  assert.ok(authenticate < activeProcessor);
});

test("every startup failure awaits full capture cleanup", () => {
  const start = source.indexOf("async function startCapture");
  const end = source.indexOf("async function consumeFrames", start);
  const startup = source.slice(start, end);
  assert.match(startup, /catch \(error\) \{\s+await stopCapture\(\);/);
});

test("reconfiguration closes connection and encoder before creating the new generation", () => {
  const start = source.indexOf("async function reconfigureForSource");
  const end = source.indexOf("async function allocateNextSourceEpoch", start);
  const reconfiguration = source.slice(start, end);
  const closeConnection = reconfiguration.indexOf("await closeReceiver()");
  const closeEncoder = reconfiguration.indexOf(
    "await stopVideoEncoder({ flush: false })"
  );
  const createEncoder = reconfiguration.indexOf(
    "await createVideoEncoder(width, height)"
  );
  const connect = reconfiguration.indexOf("await connectReceiver({");
  assert.ok(closeConnection >= 0);
  assert.ok(closeConnection < closeEncoder);
  assert.ok(closeEncoder < createEncoder);
  assert.ok(createEncoder < connect);
});
