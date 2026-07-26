import test from "node:test";
import assert from "node:assert/strict";

import {
  KEYFRAME_INTERVAL_US,
  shouldRequestPeriodicKeyFrame
} from "../keyframe-policy.js";

test("keeps periodic keyframes near two seconds for a measured 30 fps source", () => {
  let lastKeyFrameTimestampUs = Number.NaN;
  const keyFrameTimestamps = [];
  for (let frame = 0; frame <= 120; frame += 1) {
    const timestampUs = Math.round(frame * 1_000_000 / 30);
    if (shouldRequestPeriodicKeyFrame(timestampUs, lastKeyFrameTimestampUs)) {
      keyFrameTimestamps.push(timestampUs);
      lastKeyFrameTimestampUs = timestampUs;
    }
  }
  assert.deepEqual(keyFrameTimestamps, [
    0,
    KEYFRAME_INTERVAL_US,
    KEYFRAME_INTERVAL_US * 2
  ]);
});

test("requests a new keyframe when a source timestamp restarts", () => {
  assert.equal(shouldRequestPeriodicKeyFrame(500_000, 2_000_000), true);
});
