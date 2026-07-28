import test from "node:test";
import assert from "node:assert/strict";

import { DirectResyncPolicy } from "../direct-resync-policy.js";

test("a disconnected or backpressured AU drop forces the next encoded frame to key", () => {
  for (const deliveryFailure of ["disconnected", "backpressure"]) {
    const policy = new DirectResyncPolicy();
    assert.equal(policy.shouldEncodeKeyFrame(false), true);
    policy.onKeyFrameSubmitted();
    policy.onEncodedChunkDelivered("key");
    assert.equal(policy.shouldEncodeKeyFrame(false), false);

    assert.equal(policy.requireKeyFrame(), true, deliveryFailure);
    assert.equal(policy.shouldEncodeKeyFrame(false), true);
    assert.equal(policy.canDeliverEncodedChunk("delta"), false);
    assert.equal(policy.canDeliverEncodedChunk("key"), true);
    policy.onKeyFrameSubmitted();
    assert.equal(policy.canDeliverEncodedChunk("delta"), false);
    policy.onEncodedChunkDelivered("key");
    assert.equal(policy.canDeliverEncodedChunk("delta"), true);
    assert.equal(policy.resyncEvents, 1);
  }
});

test("normal continuous delivery preserves periodic keyframe behavior", () => {
  const policy = new DirectResyncPolicy();
  policy.onKeyFrameSubmitted();
  policy.onEncodedChunkDelivered("key");
  assert.equal(policy.shouldEncodeKeyFrame(false), false);
  assert.equal(policy.shouldEncodeKeyFrame(true), true);
  assert.equal(policy.resyncEvents, 0);
});

test("repeated drops before one recovery keyframe form one resync event", () => {
  const policy = new DirectResyncPolicy();
  policy.onKeyFrameSubmitted();
  policy.onEncodedChunkDelivered("key");
  assert.equal(policy.requireKeyFrame(), true);
  assert.equal(policy.requireKeyFrame(), false);
  assert.equal(policy.resyncEvents, 1);
});
