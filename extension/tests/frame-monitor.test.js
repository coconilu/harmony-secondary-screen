import test from "node:test";
import assert from "node:assert/strict";

import { FrameMonitor } from "../frame-monitor.js";

test("counts frames and estimates a 30 fps stream", () => {
  let now = 0;
  const monitor = new FrameMonitor({ now: () => now });

  for (let index = 0; index < 31; index += 1) {
    now = index * (1000 / 30);
    monitor.onFrame();
  }

  const snapshot = monitor.sample();
  assert.equal(snapshot.totalFrames, 31);
  assert.ok(snapshot.fps > 29.5 && snapshot.fps < 30.5);
  assert.equal(snapshot.stallEvents, 0);
  assert.equal(snapshot.stalled, false);
});

test("records one stall after two seconds without a frame", () => {
  let now = 0;
  const monitor = new FrameMonitor({ now: () => now });
  monitor.onFrame();

  now = 1999;
  assert.equal(monitor.sample().stalled, false);

  now = 2000;
  const stalled = monitor.sample();
  assert.equal(stalled.stalled, true);
  assert.equal(stalled.stallEvents, 1);
  assert.equal(stalled.transition, "stalled");

  now = 3000;
  const stillStalled = monitor.sample();
  assert.equal(stillStalled.stallEvents, 1);
  assert.equal(stillStalled.transition, null);
});

test("reports recovery and can detect a later independent stall", () => {
  let now = 0;
  const monitor = new FrameMonitor({ now: () => now });
  monitor.onFrame();

  now = 2100;
  monitor.sample();

  now = 2200;
  monitor.onFrame();
  const recovered = monitor.sample();
  assert.equal(recovered.stalled, false);
  assert.equal(recovered.transition, "recovered");
  assert.equal(recovered.stallEvents, 1);

  now = 4200;
  const stalledAgain = monitor.sample();
  assert.equal(stalledAgain.stalled, true);
  assert.equal(stalledAgain.stallEvents, 2);
});

test("detects a missing first frame", () => {
  let now = 10;
  const monitor = new FrameMonitor({ now: () => now });

  now = 2009;
  assert.equal(monitor.sample().stalled, false);

  now = 2010;
  const snapshot = monitor.sample();
  assert.equal(snapshot.stalled, true);
  assert.equal(snapshot.stallEvents, 1);
});
