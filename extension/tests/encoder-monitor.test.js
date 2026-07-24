import test from "node:test";
import assert from "node:assert/strict";

import { EncoderMonitor } from "../encoder-monitor.js";

test("counts encoded chunks and estimates fps and bitrate", () => {
  let now = 0;
  const monitor = new EncoderMonitor({ now: () => now });

  for (let index = 0; index < 31; index += 1) {
    now = index * (1000 / 30);
    monitor.onSubmitted();
    monitor.onChunk({
      byteLength: 16_667,
      type: index === 0 ? "key" : "delta"
    });
  }

  const snapshot = monitor.sample(1);
  assert.equal(snapshot.submittedFrames, 31);
  assert.equal(snapshot.encodedFrames, 31);
  assert.equal(snapshot.keyFrames, 1);
  assert.equal(snapshot.encodeQueueSize, 1);
  assert.ok(snapshot.encodingFps > 29.5 && snapshot.encodingFps < 30.5);
  assert.ok(snapshot.bitrateKbps > 4000 && snapshot.bitrateKbps < 4200);
  assert.ok(snapshot.averageBitrateKbps > 4000);
});

test("tracks backpressure drops and encoder errors", () => {
  const monitor = new EncoderMonitor({ now: () => 100 });
  monitor.onSubmitted();
  monitor.onDropped();
  monitor.onDropped();
  monitor.onError();

  const snapshot = monitor.sample(3);
  assert.equal(snapshot.submittedFrames, 1);
  assert.equal(snapshot.encodedFrames, 0);
  assert.equal(snapshot.droppedFrames, 2);
  assert.equal(snapshot.encodeErrors, 1);
  assert.equal(snapshot.encodeQueueSize, 3);
  assert.equal(snapshot.lastEncodedAgeMs, null);
});

test("prunes bitrate samples outside the rolling window", () => {
  let now = 0;
  const monitor = new EncoderMonitor({ now: () => now });
  monitor.onChunk({ byteLength: 1000, type: "key" });

  now = 1001;
  const snapshot = monitor.sample();
  assert.equal(snapshot.bitrateKbps, 0);
  assert.equal(snapshot.encodingFps, 0);
  assert.equal(snapshot.encodedBytes, 1000);
});
