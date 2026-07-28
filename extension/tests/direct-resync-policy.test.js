import test from "node:test";
import assert from "node:assert/strict";

import { DirectDeliveryOrchestrator } from "../direct-delivery-orchestrator.js";
import { DirectResyncPolicy } from "../direct-resync-policy.js";

function chunk(type, bytes = [0, 0, 0, 1, 0x65]) {
  const payload = Uint8Array.from(bytes);
  return {
    type,
    timestamp: 123,
    byteLength: payload.byteLength,
    copyTo(destination) {
      destination.set(payload);
    }
  };
}

function telemetry() {
  return {
    directSentFrames: 0,
    directSentBytes: 0,
    directDroppedFrames: 0,
    directResyncEvents: 0
  };
}

test("offscreen delivery orchestration blocks delta until a key AU is sent", () => {
  const delivery = new DirectDeliveryOrchestrator();
  const counters = telemetry();
  let sendCalls = 0;
  let sendResult = true;
  const connected = {
    connected: true,
    sendVideo() {
      sendCalls += 1;
      return sendResult;
    }
  };

  delivery.onKeyFrameSubmitted();
  assert.equal(delivery.deliver(chunk("key"), connected, 7, counters), true);
  assert.equal(delivery.sequence, 1);
  assert.equal(counters.directSentFrames, 1);

  const disconnected = {
    connected: false,
    sendVideo() {
      throw new Error("disconnected transport must not be called");
    }
  };
  assert.equal(
    delivery.deliver(chunk("delta"), disconnected, 7, counters),
    false
  );
  assert.equal(delivery.sequence, 1);
  assert.equal(counters.directSentFrames, 1);
  assert.equal(counters.directResyncEvents, 1);

  const callsBeforeBlockedDelta = sendCalls;
  assert.equal(delivery.deliver(chunk("delta"), connected, 7, counters), false);
  assert.equal(sendCalls, callsBeforeBlockedDelta);
  assert.equal(delivery.sequence, 1);
  assert.equal(counters.directSentFrames, 1);

  delivery.onKeyFrameSubmitted();
  sendResult = false;
  assert.equal(delivery.deliver(chunk("key"), connected, 7, counters), false);
  assert.equal(delivery.sequence, 1);
  assert.equal(counters.directSentFrames, 1);
  assert.equal(
    delivery.deliver(chunk("delta"), connected, 7, counters),
    false
  );
  assert.equal(delivery.sequence, 1);
  assert.equal(counters.directSentFrames, 1);

  delivery.onKeyFrameSubmitted();
  sendResult = true;
  assert.equal(delivery.deliver(chunk("key"), connected, 7, counters), true);
  assert.equal(delivery.sequence, 2);
  assert.equal(counters.directSentFrames, 2);
  assert.equal(delivery.deliver(chunk("delta"), connected, 7, counters), true);
  assert.equal(delivery.sequence, 3);
  assert.equal(counters.directSentFrames, 3);
  assert.equal(counters.directDroppedFrames, 4);
});

test("offscreen delivery orchestration preserves periodic keyframe behavior", () => {
  const delivery = new DirectDeliveryOrchestrator();
  delivery.onKeyFrameSubmitted();
  assert.equal(delivery.shouldEncodeKeyFrame(false), false);
  assert.equal(delivery.shouldEncodeKeyFrame(true), true);
});

test("offscreen delivery reset starts a new sequence and sync gate", () => {
  const delivery = new DirectDeliveryOrchestrator();
  const counters = telemetry();
  const connected = {
    connected: true,
    sendVideo() {
      return true;
    }
  };
  delivery.onKeyFrameSubmitted();
  delivery.deliver(chunk("key"), connected, 7, counters);
  assert.equal(delivery.sequence, 1);
  delivery.reset();
  assert.equal(delivery.sequence, 0);
  const callsBeforeDelta = counters.directSentFrames;
  assert.equal(delivery.deliver(chunk("delta"), connected, 8, counters), false);
  assert.equal(counters.directSentFrames, callsBeforeDelta);
  assert.equal(delivery.sequence, 0);
});

test("policy repeated drops before one recovery keyframe form one resync event", () => {
  const policy = new DirectResyncPolicy();
  policy.onKeyFrameSubmitted();
  policy.onEncodedChunkDelivered("key");
  assert.equal(policy.requireKeyFrame(), true);
  assert.equal(policy.requireKeyFrame(), false);
  assert.equal(policy.resyncEvents, 1);
});
