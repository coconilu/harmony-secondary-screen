import test from "node:test";
import assert from "node:assert/strict";

import {
  buildAutoVideoContractCandidates,
  calculateAutoBitrate,
  configureAutoVideoEncoder,
  createEncoderBinding,
  isCurrentEncoderBinding,
  MaxFrameRateGate,
  selectAutoDimensions,
  StableFrameSizeTracker,
  validateMediaContract,
  VIDEO_MAX_BITRATE,
  VIDEO_MAX_PIXELS,
  VIDEO_MIN_BITRATE
} from "../video-contract.js";

const REQUIRED_SIZES = [
  [640, 480, 640, 480],
  [1280, 720, 1280, 720],
  [1365, 768, 1364, 768],
  [1920, 1080, 1920, 1080],
  [3840, 2160, 1920, 1080],
  [1080, 1920, 1080, 1920],
  [641, 479, 640, 478]
];

test("automatic dimensions preserve direction, never upscale and stay even within 1080p", () => {
  for (const [sourceWidth, sourceHeight, width, height] of REQUIRED_SIZES) {
    const selected = selectAutoDimensions(sourceWidth, sourceHeight);
    assert.deepEqual(
      [selected.width, selected.height],
      [width, height],
      `${sourceWidth}x${sourceHeight}`
    );
    assert.equal(selected.width % 2, 0);
    assert.equal(selected.height % 2, 0);
    assert.ok(selected.width <= sourceWidth);
    assert.ok(selected.height <= sourceHeight);
    assert.ok(selected.width * selected.height <= VIDEO_MAX_PIXELS);
    assert.ok(
      Math.abs(
        selected.width / selected.height - sourceWidth / sourceHeight
      ) <= 2 / Math.min(selected.width, selected.height)
    );
  }
});

test("automatic dimensions cover high-resolution 4:3, tiny and non-finite inputs", () => {
  assert.deepEqual(
    selectAutoDimensions(4000, 3000),
    {
      width: 1440,
      height: 1080,
      sourceWidth: 4000,
      sourceHeight: 3000,
      scale: 0.36
    }
  );
  assert.deepEqual(
    selectAutoDimensions(2, 2),
    {
      width: 2,
      height: 2,
      sourceWidth: 2,
      sourceHeight: 2,
      scale: 1
    }
  );
  for (const value of [NaN, Infinity, -Infinity, 1, 0]) {
    assert.throws(() => selectAutoDimensions(value, 720));
    assert.throws(() => selectAutoDimensions(1280, value));
  }
});

test("automatic bitrate remains bounded and follows pixels and max fps", () => {
  const low = calculateAutoBitrate(640, 480, 24);
  const high = calculateAutoBitrate(1920, 1080, 60);
  assert.ok(low >= VIDEO_MIN_BITRATE);
  assert.ok(high <= VIDEO_MAX_BITRATE);
  assert.ok(high > low);
});

test("candidate list keeps low resolutions and adds a <=720p fallback for high sources", () => {
  const low = buildAutoVideoContractCandidates({
    sourceWidth: 640,
    sourceHeight: 480,
    settingsFrameRate: 30
  });
  assert.equal(low.length, 1);
  assert.deepEqual(
    [low[0].width, low[0].height, low[0].maxFps],
    [640, 480, 30]
  );

  const high = buildAutoVideoContractCandidates({
    sourceWidth: 3840,
    sourceHeight: 2160,
    settingsFrameRate: 120
  });
  assert.equal(high.length, 2);
  assert.deepEqual(
    [high[0].width, high[0].height, high[0].maxFps],
    [1920, 1080, 60]
  );
  assert.equal(high[0].codec, "avc1.42002a");
  assert.deepEqual(
    [high[1].width, high[1].height],
    [1280, 720]
  );
  assert.equal(high[1].codec, "avc1.420028");
});

test("capability rejection and configure failure both fall back safely", async () => {
  const candidates = buildAutoVideoContractCandidates({
    sourceWidth: 1920,
    sourceHeight: 1080,
    settingsFrameRate: 60
  });
  const configured = [];
  const result = await configureAutoVideoEncoder({
    candidates,
    isConfigSupported: async () => ({ supported: true }),
    createEncoder: () => ({
      configure(config) {
        configured.push([config.width, config.height]);
        if (config.width > 1280) {
          throw new Error("simulated primary configure failure");
        }
      },
      close() {}
    }),
    output() {},
    error() {}
  });
  assert.deepEqual(configured, [[1920, 1080], [1280, 720]]);
  assert.equal(result.contract.selection, "fallback");
  assert.ok(result.contract.width <= 1280);
  assert.ok(result.contract.height <= 720);
});

test("normalized capability output cannot change the authenticated contract", async () => {
  const [contract] = buildAutoVideoContractCandidates({
    sourceWidth: 640,
    sourceHeight: 480,
    settingsFrameRate: 30
  });
  const result = await configureAutoVideoEncoder({
    candidates: [contract],
    isConfigSupported: async () => ({
      supported: true,
      config: {
        width: 1920,
        height: 1080,
        framerate: 60,
        codec: "avc1.64002a"
      }
    }),
    createEncoder: () => ({
      configure() {},
      close() {}
    }),
    output() {},
    error() {}
  });
  assert.deepEqual(
    [
      result.config.width,
      result.config.height,
      result.config.framerate,
      result.config.codec
    ],
    [640, 480, 30, contract.codec]
  );
});

test("all unsupported candidates produce a user-readable terminal failure", async () => {
  const candidates = buildAutoVideoContractCandidates({
    sourceWidth: 1920,
    sourceHeight: 1080,
    settingsFrameRate: 60
  });
  await assert.rejects(configureAutoVideoEncoder({
    candidates,
    isConfigSupported: async () => ({ supported: false }),
    createEncoder: () => {
      throw new Error("must not create an unsupported encoder");
    },
    output() {},
    error() {}
  }), /不高于 720p 的回退/);
});

test("24, 30, 50 and 60 fps inputs submit only their existing frames", () => {
  for (const fps of [24, 30, 50, 60]) {
    const gate = new MaxFrameRateGate(60);
    let submitted = 0;
    for (let frame = 0; frame < fps * 2; frame += 1) {
      if (gate.shouldSubmit(Math.round(frame * 1_000_000 / fps))) {
        submitted += 1;
      }
    }
    assert.equal(submitted, fps * 2, `${fps} fps`);
  }
});

test("inputs above 60 fps are capped without creating replacement frames", () => {
  const gate = new MaxFrameRateGate(60);
  const inputTimestamps = Array.from(
    { length: 240 },
    (_, index) => Math.round(index * 1_000_000 / 120)
  );
  const submitted = inputTimestamps.filter(
    (timestamp) => gate.shouldSubmit(timestamp)
  );
  assert.ok(submitted.length <= 121);
  assert.ok(submitted.length >= 119);
  assert.ok(submitted.every((timestamp) => inputTimestamps.includes(timestamp)));
});

test("timestamp rollback resets the fps gate without inventing a frame", () => {
  const gate = new MaxFrameRateGate(60);
  assert.equal(gate.shouldSubmit(1_000_000), true);
  assert.equal(gate.shouldSubmit(1_008_000), false);
  assert.equal(gate.shouldSubmit(20_000), true);
  assert.equal(gate.shouldSubmit(28_000), false);
  assert.equal(gate.shouldSubmit(37_000), true);
});

test("one thousand duplicate timestamps cannot bypass the max fps gate", () => {
  const gate = new MaxFrameRateGate(60);
  assert.equal(gate.shouldSubmit(1_000_000), true);
  let duplicatesSubmitted = 0;
  for (let duplicate = 0; duplicate < 1_000; duplicate += 1) {
    if (gate.shouldSubmit(1_000_000)) {
      duplicatesSubmitted += 1;
    }
  }
  assert.equal(duplicatesSubmitted, 0);
  assert.equal(gate.shouldSubmit(1_016_667), true);
});

test("stable source size changes trigger once and transient changes do not", () => {
  const tracker = new StableFrameSizeTracker(1280, 720, 3);
  assert.equal(tracker.observe(1920, 1080), null);
  assert.equal(tracker.observe(1280, 720), null);
  assert.equal(tracker.observe(1920, 1080), null);
  assert.equal(tracker.observe(1920, 1080), null);
  assert.deepEqual(tracker.observe(1920, 1080), {
    width: 1920,
    height: 1080
  });
  assert.equal(tracker.observe(1920, 1080), null);
  tracker.commit(1920, 1080);
  assert.equal(tracker.observe(1920, 1080), null);
});

test("A to B to A jitter and B to C pending observations cannot overwrite a committed size", () => {
  const tracker = new StableFrameSizeTracker(1280, 720, 3);
  tracker.observe(1920, 1080);
  tracker.observe(1280, 720);
  assert.equal(tracker.observe(1280, 720), null);
  assert.equal(tracker.observe(1600, 900), null);
  assert.equal(tracker.observe(1440, 900), null);
  assert.equal(tracker.observe(1440, 900), null);
  assert.deepEqual(tracker.observe(1440, 900), {
    width: 1440,
    height: 900
  });
  tracker.commit(1440, 900);
  assert.equal(tracker.observe(1440, 900), null);
});

test("immutable encoder bindings reject old generations, epochs and contracts", () => {
  const binding = createEncoderBinding(4, 9, {
    width: 1920,
    height: 1080,
    maxFps: 60
  });
  assert.equal(Object.isFrozen(binding), true);
  assert.equal(Object.isFrozen(binding.contract), true);
  assert.equal(isCurrentEncoderBinding(binding, 4, 9, binding.contract), true);
  assert.equal(isCurrentEncoderBinding(binding, 5, 9, binding.contract), false);
  assert.equal(isCurrentEncoderBinding(binding, 4, 10, binding.contract), false);
  assert.equal(isCurrentEncoderBinding(binding, 4, 9, {
    width: 1280,
    height: 720,
    maxFps: 60
  }), false);
});

test("Receiver media contract boundary rejects malformed or excessive values", () => {
  for (const value of [
    { width: 0, height: 720, maxFps: 60 },
    { width: -2, height: 720, maxFps: 60 },
    { width: 1279, height: 720, maxFps: 60 },
    { width: 1922, height: 1080, maxFps: 60 },
    { width: 1920, height: 1082, maxFps: 60 },
    { width: 1920, height: 1080, maxFps: 61 }
  ]) {
    assert.throws(() => validateMediaContract(value));
  }
  assert.deepEqual(
    validateMediaContract({ width: 1080, height: 1920, maxFps: 60 }),
    { width: 1080, height: 1920, maxFps: 60 }
  );
});
