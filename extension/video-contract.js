export const VIDEO_MAX_LONG_EDGE = 1920;
export const VIDEO_MAX_SHORT_EDGE = 1080;
export const VIDEO_MAX_PIXELS = 1920 * 1080;
export const VIDEO_FALLBACK_MAX_LONG_EDGE = 1280;
export const VIDEO_FALLBACK_MAX_SHORT_EDGE = 720;
export const VIDEO_FALLBACK_MAX_PIXELS = 1280 * 720;
export const VIDEO_MAX_FPS = 60;
export const VIDEO_MIN_BITRATE = 750_000;
export const VIDEO_MAX_BITRATE = 12_000_000;
export const VIDEO_DIMENSION_STABILITY_FRAMES = 3;

const BITRATE_BITS_PER_PIXEL_FRAME = 0.1;
const BITRATE_ROUNDING = 100_000;

export function selectAutoDimensions(
  width,
  height,
  {
    maxLongEdge = VIDEO_MAX_LONG_EDGE,
    maxShortEdge = VIDEO_MAX_SHORT_EDGE,
    maxPixels = VIDEO_MAX_PIXELS
  } = {}
) {
  const source = normalizeSourceSize(width, height);
  if (
    !Number.isInteger(maxLongEdge) ||
    !Number.isInteger(maxShortEdge) ||
    !Number.isInteger(maxPixels) ||
    maxLongEdge < 2 ||
    maxShortEdge < 2 ||
    maxPixels < 4
  ) {
    throw new Error("自动画面尺寸上限无效");
  }

  const sourceLong = Math.max(source.width, source.height);
  const sourceShort = Math.min(source.width, source.height);
  const scale = Math.min(
    1,
    maxLongEdge / sourceLong,
    maxShortEdge / sourceShort,
    Math.sqrt(maxPixels / (source.width * source.height))
  );
  const targetWidth = evenFloor(source.width * scale);
  const targetHeight = evenFloor(source.height * scale);
  if (targetWidth < 2 || targetHeight < 2) {
    throw new Error("捕获画面尺寸过小，无法生成偶数 H.264 画面");
  }
  return {
    width: targetWidth,
    height: targetHeight,
    sourceWidth: source.width,
    sourceHeight: source.height,
    scale
  };
}

export function deriveMaxFps(settingsFrameRate) {
  if (!Number.isFinite(settingsFrameRate) || settingsFrameRate <= 0) {
    return VIDEO_MAX_FPS;
  }
  return Math.max(
    1,
    Math.min(VIDEO_MAX_FPS, Math.round(settingsFrameRate))
  );
}

export function calculateAutoBitrate(width, height, maxFps) {
  const contract = validateMediaContract({ width, height, maxFps });
  const raw = contract.width * contract.height * contract.maxFps *
    BITRATE_BITS_PER_PIXEL_FRAME;
  const rounded = Math.round(raw / BITRATE_ROUNDING) * BITRATE_ROUNDING;
  return Math.max(
    VIDEO_MIN_BITRATE,
    Math.min(VIDEO_MAX_BITRATE, rounded)
  );
}

export function buildAutoVideoContractCandidates({
  sourceWidth,
  sourceHeight,
  settingsFrameRate
}) {
  const maxFps = deriveMaxFps(settingsFrameRate);
  const preferred = createContract(
    selectAutoDimensions(sourceWidth, sourceHeight),
    maxFps,
    "preferred"
  );
  const fallback = createContract(
    selectAutoDimensions(sourceWidth, sourceHeight, {
      maxLongEdge: VIDEO_FALLBACK_MAX_LONG_EDGE,
      maxShortEdge: VIDEO_FALLBACK_MAX_SHORT_EDGE,
      maxPixels: VIDEO_FALLBACK_MAX_PIXELS
    }),
    maxFps,
    "fallback"
  );
  return preferred.width === fallback.width &&
    preferred.height === fallback.height
    ? [preferred]
    : [preferred, fallback];
}

export function validateMediaContract(value) {
  const width = value?.width;
  const height = value?.height;
  const maxFps = value?.maxFps;
  if (
    !Number.isInteger(width) ||
    !Number.isInteger(height) ||
    !Number.isInteger(maxFps) ||
    width <= 0 ||
    height <= 0 ||
    width % 2 !== 0 ||
    height % 2 !== 0 ||
    Math.max(width, height) > VIDEO_MAX_LONG_EDGE ||
    Math.min(width, height) > VIDEO_MAX_SHORT_EDGE ||
    width * height > VIDEO_MAX_PIXELS ||
    maxFps <= 0 ||
    maxFps > VIDEO_MAX_FPS
  ) {
    throw new Error("动态视频合同超出可信媒体边界");
  }
  return { width, height, maxFps };
}

export function encoderConfigForContract(contract) {
  const normalized = validateMediaContract(contract);
  const bitrate = Number.isInteger(contract?.bitrate)
    ? contract.bitrate
    : calculateAutoBitrate(
      normalized.width,
      normalized.height,
      normalized.maxFps
    );
  if (bitrate < VIDEO_MIN_BITRATE || bitrate > VIDEO_MAX_BITRATE) {
    throw new Error("自动目标码率超出安全边界");
  }
  return {
    codec: contract.codec ?? codecForContract(normalized),
    width: normalized.width,
    height: normalized.height,
    bitrate,
    framerate: normalized.maxFps,
    hardwareAcceleration: "prefer-hardware",
    latencyMode: "realtime",
    alpha: "discard",
    avc: {
      format: "annexb"
    }
  };
}

export async function configureAutoVideoEncoder({
  candidates,
  isConfigSupported,
  createEncoder,
  output,
  error
}) {
  if (
    !Array.isArray(candidates) ||
    candidates.length === 0 ||
    typeof isConfigSupported !== "function" ||
    typeof createEncoder !== "function"
  ) {
    throw new Error("H.264 自动能力探测参数无效");
  }
  for (const contract of candidates) {
    const requestedConfig = encoderConfigForContract(contract);
    let support;
    try {
      support = await isConfigSupported(requestedConfig);
    } catch {
      continue;
    }
    if (!support?.supported) {
      continue;
    }
    const supportedConfig = {
      ...requestedConfig,
      ...(support.config ?? {}),
      codec: requestedConfig.codec,
      width: requestedConfig.width,
      height: requestedConfig.height,
      bitrate: requestedConfig.bitrate,
      framerate: requestedConfig.framerate,
      avc: {
        ...requestedConfig.avc,
        ...(support.config?.avc ?? {}),
        format: "annexb"
      }
    };
    const encoder = createEncoder({
      output,
      error,
      contract: {
        ...contract,
        bitrate: requestedConfig.bitrate
      },
      config: supportedConfig
    });
    try {
      encoder.configure(supportedConfig);
      return {
        encoder,
        contract: {
          ...contract,
          bitrate: requestedConfig.bitrate
        },
        config: supportedConfig
      };
    } catch {
      try {
        encoder.close();
      } catch {
        // A configure failure can leave a WebCodecs encoder already closed.
      }
    }
  }
  throw new Error(
    "当前 Edge 无法创建安全的 H.264 编码配置；已尝试自动分辨率和不高于 720p 的回退"
  );
}

export class MaxFrameRateGate {
  constructor(maxFps) {
    this.maxFps = validateMediaContract({
      width: 2,
      height: 2,
      maxFps
    }).maxFps;
    this.lastAcceptedTimestampUs = null;
  }

  shouldSubmit(timestampUs) {
    if (!Number.isFinite(timestampUs) || timestampUs < 0) {
      return true;
    }
    if (
      this.lastAcceptedTimestampUs === null ||
      timestampUs <= this.lastAcceptedTimestampUs
    ) {
      this.lastAcceptedTimestampUs = timestampUs;
      return true;
    }
    const minimumIntervalUs = 1_000_000 / this.maxFps;
    if (
      timestampUs - this.lastAcceptedTimestampUs + 1 <
      minimumIntervalUs
    ) {
      return false;
    }
    this.lastAcceptedTimestampUs = timestampUs;
    return true;
  }
}

export class StableFrameSizeTracker {
  constructor(
    width,
    height,
    threshold = VIDEO_DIMENSION_STABILITY_FRAMES
  ) {
    const source = normalizeSourceSize(width, height);
    if (!Number.isInteger(threshold) || threshold < 2) {
      throw new Error("稳定尺寸确认帧数无效");
    }
    this.width = source.width;
    this.height = source.height;
    this.threshold = threshold;
    this.pendingWidth = 0;
    this.pendingHeight = 0;
    this.pendingCount = 0;
    this.triggered = false;
  }

  observe(width, height) {
    const source = normalizeSourceSize(width, height);
    if (source.width === this.width && source.height === this.height) {
      this.resetPending();
      return null;
    }
    if (
      source.width !== this.pendingWidth ||
      source.height !== this.pendingHeight
    ) {
      this.pendingWidth = source.width;
      this.pendingHeight = source.height;
      this.pendingCount = 1;
      this.triggered = false;
      return null;
    }
    this.pendingCount += 1;
    if (this.pendingCount < this.threshold || this.triggered) {
      return null;
    }
    this.triggered = true;
    return {
      width: this.pendingWidth,
      height: this.pendingHeight
    };
  }

  commit(width, height) {
    const source = normalizeSourceSize(width, height);
    this.width = source.width;
    this.height = source.height;
    this.resetPending();
  }

  resetPending() {
    this.pendingWidth = 0;
    this.pendingHeight = 0;
    this.pendingCount = 0;
    this.triggered = false;
  }
}

export function createEncoderBinding(generation, sourceEpoch, contract) {
  if (
    !Number.isInteger(generation) ||
    generation <= 0 ||
    !Number.isInteger(sourceEpoch) ||
    sourceEpoch <= 0
  ) {
    throw new Error("编码器代际或来源 epoch 无效");
  }
  return Object.freeze({
    generation,
    sourceEpoch,
    contract: Object.freeze(validateMediaContract(contract))
  });
}

export function isCurrentEncoderBinding(
  binding,
  generation,
  sourceEpoch,
  contract
) {
  if (!binding || binding.generation !== generation ||
      binding.sourceEpoch !== sourceEpoch) {
    return false;
  }
  let normalized;
  try {
    normalized = validateMediaContract(contract);
  } catch {
    return false;
  }
  return binding.contract.width === normalized.width &&
    binding.contract.height === normalized.height &&
    binding.contract.maxFps === normalized.maxFps;
}

function createContract(dimensions, maxFps, selection) {
  const contract = validateMediaContract({
    width: dimensions.width,
    height: dimensions.height,
    maxFps
  });
  return {
    ...contract,
    codec: codecForContract(contract),
    bitrate: calculateAutoBitrate(
      contract.width,
      contract.height,
      contract.maxFps
    ),
    sourceWidth: dimensions.sourceWidth,
    sourceHeight: dimensions.sourceHeight,
    selection
  };
}

function codecForContract(contract) {
  // AVC Level 4.0 is not sufficient for 1080p60 macroblock throughput.
  // Level 4.2 covers the preferred ceiling; the <=720p fallback keeps the
  // older Level 4.0 request for broader hardware compatibility.
  return contract.width * contract.height * contract.maxFps >
    1280 * 720 * VIDEO_MAX_FPS
    ? "avc1.42002a"
    : "avc1.420028";
}

function normalizeSourceSize(width, height) {
  if (
    !Number.isFinite(width) ||
    !Number.isFinite(height) ||
    width < 2 ||
    height < 2
  ) {
    throw new Error("捕获画面尺寸无效");
  }
  return {
    width: Math.floor(width),
    height: Math.floor(height)
  };
}

function evenFloor(value) {
  return Math.floor(value / 2) * 2;
}
