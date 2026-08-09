import { FrameMonitor } from "./frame-monitor.js";
import { EncoderMonitor } from "./encoder-monitor.js";
import {
  DIRECT_RECOVERY_CONNECT_TIMEOUT_MS,
  DIRECT_RECOVERY_RETRY_DELAYS_MS,
  DirectReceiverConnection,
  ReceiverRecoveryCancelledError
} from "./direct-client.js";
import { DirectDeliveryOrchestrator } from "./direct-delivery-orchestrator.js";
import { shouldRequestPeriodicKeyFrame } from "./keyframe-policy.js";
import {
  buildAutoVideoContractCandidates,
  configureAutoVideoEncoder,
  createEncoderBinding,
  isCurrentEncoderBinding,
  MaxFrameRateGate,
  StableFrameSizeTracker,
  validateMediaContract,
  VIDEO_MAX_FPS
} from "./video-contract.js";

const TELEMETRY_INTERVAL_MS = 500;
const MAX_ENCODE_QUEUE_SIZE = 2;

let mediaStream = null;
let videoTrack = null;
let captureSettings = null;
let frameReader = null;
let frameLoopPromise = null;
let telemetryTimer = null;
let monitor = null;
let encoderMonitor = null;
let videoEncoder = null;
let encoderConfig = null;
let mediaContract = null;
let sourceDimensions = null;
let frameRateGate = null;
let dimensionTracker = null;
let activeEncoderGeneration = 0;
let encodeCanvas = null;
let encodeContext = null;
let receiverConnection = null;
let receiverRecoveryGeneration = 0;
let receiverRecoveryPromise = null;
let receiverRecoveryAbortController = null;
let sourceEpoch = 0;
let trustedDevice = null;
let directTelemetry = createDirectTelemetry();
const directDelivery = new DirectDeliveryOrchestrator();
let lastKeyFrameTimestampUs = Number.NaN;
let running = false;
let stopping = false;
let reconfiguring = false;

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.target !== "offscreen") {
    return false;
  }

  if (message.type === "START_CAPTURE") {
    startCapture(message.streamId, message.directInfo)
      .then((telemetry) => sendResponse({ ok: true, telemetry }))
      .catch((error) => sendResponse({ ok: false, error: normalizeError(error) }));
    return true;
  }

  if (message.type === "STOP_CAPTURE") {
    stopCapture()
      .then((telemetry) => sendResponse({ ok: true, telemetry }))
      .catch((error) => sendResponse({ ok: false, error: normalizeError(error) }));
    return true;
  }

  if (message.type === "GET_OFFSCREEN_STATUS") {
    sendResponse({
      ok: true,
      running,
      telemetry: collectTelemetry()
    });
    return false;
  }

  return false;
});

async function startCapture(streamId, directInfo) {
  if (!streamId) {
    throw new Error("缺少标签页媒体流标识");
  }

  await stopCapture();
  try {
    stopping = false;
    reconfiguring = false;
    directTelemetry = createDirectTelemetry();
    directDelivery.reset();
    lastKeyFrameTimestampUs = Number.NaN;
    if (
      !directInfo ||
      !Number.isInteger(directInfo.sourceEpoch) ||
      directInfo.sourceEpoch <= 0
    ) {
      throw new Error("平板连接信息无效");
    }
    sourceEpoch = directInfo.sourceEpoch;
    trustedDevice = directInfo.trustedDevice;

    mediaStream = await navigator.mediaDevices.getUserMedia({
      audio: false,
      video: {
        mandatory: {
          chromeMediaSource: "tab",
          chromeMediaSourceId: streamId,
          maxFrameRate: VIDEO_MAX_FPS
        }
      }
    });

    const tracks = mediaStream.getVideoTracks();
    if (tracks.length !== 1) {
      throw new Error(`预期 1 条视频轨，实际得到 ${tracks.length} 条`);
    }
    if (mediaStream.getAudioTracks().length !== 0) {
      throw new Error("捕获流意外包含音频轨");
    }
    if (typeof MediaStreamTrackProcessor !== "function") {
      throw new Error("当前 Edge 不支持 MediaStreamTrackProcessor");
    }
    if (typeof VideoEncoder !== "function") {
      throw new Error("当前 Edge 不支持 WebCodecs VideoEncoder");
    }

    videoTrack = tracks[0];
    try {
      await videoTrack.applyConstraints({
        frameRate: { max: VIDEO_MAX_FPS }
      });
    } catch {
      // legacy maxFrameRate remains the fallback constraint.
    }
    captureSettings = videoTrack.getSettings();
    videoTrack.addEventListener("ended", handleTrackEnded, { once: true });
    const processor = new MediaStreamTrackProcessor({ track: videoTrack });
    frameReader = processor.readable.getReader();
    const first = await frameReader.read();
    if (first.done || !first.value) {
      throw new Error("Edge 捕获轨没有产生可确认尺寸的视频帧");
    }
    let firstWidth;
    let firstHeight;
    try {
      firstWidth = frameWidth(first.value);
      firstHeight = frameHeight(first.value);
    } finally {
      // Never retain a picture while capability probing or authentication waits.
      first.value.close();
    }
    try {
      await frameReader.cancel();
    } catch {
      // The one-frame sizing processor may already have closed itself.
    }
    frameReader = null;
    sourceDimensions = { width: firstWidth, height: firstHeight };
    dimensionTracker = new StableFrameSizeTracker(firstWidth, firstHeight);
    encoderMonitor = new EncoderMonitor();
    directTelemetry.captureSettingsWidth =
      positiveIntegerOrNull(captureSettings.width);
    directTelemetry.captureSettingsHeight =
      positiveIntegerOrNull(captureSettings.height);
    directTelemetry.initialFrameMatchedSettings =
      directTelemetry.captureSettingsWidth === null ||
      directTelemetry.captureSettingsHeight === null
        ? null
        : directTelemetry.captureSettingsWidth === firstWidth &&
          directTelemetry.captureSettingsHeight === firstHeight;

    await createVideoEncoder(firstWidth, firstHeight);
    await connectReceiver({
      trustedDevice,
      sourceEpoch
    }, mediaContract);
    const activeProcessor = new MediaStreamTrackProcessor({
      track: videoTrack
    });
    frameReader = activeProcessor.readable.getReader();
    monitor = new FrameMonitor();
    running = true;
    frameLoopPromise = consumeFrames();
    telemetryTimer = setInterval(publishTelemetry, TELEMETRY_INTERVAL_MS);
    return collectTelemetry();
  } catch (error) {
    await stopCapture();
    throw error;
  }
}

async function consumeFrames() {
  try {
    while (running && frameReader) {
      const { value: frame, done } = await frameReader.read();
      if (done || !frame) {
        break;
      }

      monitor?.onFrame();
      const width = frameWidth(frame);
      const height = frameHeight(frame);
      const stableChange = dimensionTracker?.observe(width, height) ?? null;
      if (stableChange) {
        frame.close();
        await reconfigureForSource(stableChange.width, stableChange.height);
        continue;
      }
      if (
        sourceDimensions &&
        (width !== sourceDimensions.width || height !== sourceDimensions.height)
      ) {
        encoderMonitor?.onDropped();
        frame.close();
        continue;
      }
      encodeFrame(frame);
    }
  } catch (error) {
    if (!stopping) {
      running = false;
      await sendToServiceWorker("CAPTURE_FAILURE", {
        error: normalizeError(error),
        telemetry: collectTelemetry()
      });
      queueMicrotask(() => {
        void stopCapture();
      });
    }
  } finally {
    if (!stopping && running) {
      running = false;
      await sendToServiceWorker("CAPTURE_ENDED", {
        reason: "video_track_ended",
        telemetry: collectTelemetry()
      });
      queueMicrotask(() => {
        void stopCapture();
      });
    }
  }
}

function publishTelemetry() {
  if (!running || !monitor) {
    return;
  }

  void sendToServiceWorker("CAPTURE_TELEMETRY", {
    telemetry: collectTelemetry()
  });
}

async function stopCapture() {
  if (
    !running &&
    !mediaStream &&
    !frameReader &&
    !receiverConnection &&
    !videoEncoder
  ) {
    return monitor?.sample() ?? null;
  }

  stopping = true;
  running = false;
  receiverRecoveryGeneration += 1;
  receiverRecoveryAbortController?.abort();

  if (telemetryTimer !== null) {
    clearInterval(telemetryTimer);
    telemetryTimer = null;
  }

  const reader = frameReader;
  frameReader = null;
  if (reader) {
    try {
      await reader.cancel();
    } catch {
      // The track may already have ended.
    }
  }

  for (const track of mediaStream?.getTracks() ?? []) {
    track.stop();
  }
  mediaStream = null;
  videoTrack = null;
  captureSettings = null;

  try {
    await frameLoopPromise;
  } catch {
    // consumeFrames reports unexpected failures itself.
  }
  frameLoopPromise = null;

  await stopVideoEncoder();
  await closeReceiver();
  mediaContract = null;
  sourceDimensions = null;
  frameRateGate = null;
  dimensionTracker = null;
  trustedDevice = null;
  reconfiguring = false;
  const telemetry = collectTelemetry();
  stopping = false;
  return telemetry;
}

async function handleTrackEnded() {
  if (stopping) {
    return;
  }
  running = false;
  await sendToServiceWorker("CAPTURE_ENDED", {
    reason: "video_track_ended",
    telemetry: collectTelemetry()
  });
  queueMicrotask(() => {
    void stopCapture();
  });
}

async function createVideoEncoder(sourceWidth, sourceHeight) {
  const candidates = buildAutoVideoContractCandidates({
    sourceWidth,
    sourceHeight,
    settingsFrameRate: captureSettings?.frameRate
  });
  const result = await configureAutoVideoEncoder({
    candidates,
    isConfigSupported: (config) => VideoEncoder.isConfigSupported(config),
    createEncoder: ({ contract }) => {
      const binding = createEncoderBinding(
        ++activeEncoderGeneration,
        sourceEpoch,
        contract
      );
      return new VideoEncoder({
        output: (chunk) => {
          handleEncodedChunk(chunk, binding);
        },
        error: (error) => {
          if (binding.generation !== activeEncoderGeneration) {
            return;
          }
          encoderMonitor?.onError();
          if (!stopping) {
            running = false;
            void sendToServiceWorker("CAPTURE_FAILURE", {
              error: `H.264 编码失败：${normalizeError(error)}`,
              telemetry: collectTelemetry()
            });
            queueMicrotask(() => {
              void stopCapture();
            });
          }
        }
      });
    }
  });
  videoEncoder = result.encoder;
  mediaContract = validateMediaContract(result.contract);
  encoderConfig = {
    ...normalizeEncoderConfig(result.config),
    selectionMode: "auto",
    selection: result.contract.selection,
    maxFps: result.contract.maxFps,
    sourceWidth,
    sourceHeight
  };
  frameRateGate = new MaxFrameRateGate(mediaContract.maxFps);
  encodeCanvas = new OffscreenCanvas(
    mediaContract.width,
    mediaContract.height
  );
  encodeContext = encodeCanvas.getContext("2d", {
    alpha: false,
    desynchronized: true
  });
  if (!encodeContext) {
    throw new Error("无法创建 H.264 编码缩放画布");
  }
}

function encodeFrame(sourceFrame) {
  try {
    if (!videoEncoder || videoEncoder.state !== "configured" || !encoderMonitor) {
      throw new Error("H.264 编码器尚未就绪");
    }

    if (videoEncoder.encodeQueueSize >= MAX_ENCODE_QUEUE_SIZE) {
      encoderMonitor.onDropped();
      return;
    }

    const sourceWidth = frameWidth(sourceFrame);
    const sourceHeight = frameHeight(sourceFrame);
    if (
      !sourceDimensions ||
      sourceWidth !== sourceDimensions.width ||
      sourceHeight !== sourceDimensions.height
    ) {
      throw new Error("捕获帧尺寸与当前自动视频合同不一致");
    }

    const timestamp =
      sourceFrame.timestamp ?? Math.round(performance.now() * 1000);
    if (!frameRateGate?.shouldSubmit(timestamp)) {
      encoderMonitor.onDropped();
      return;
    }
    encodeContext.drawImage(
      sourceFrame,
      0,
      0,
      mediaContract.width,
      mediaContract.height
    );

    const encodeFrame = new VideoFrame(encodeCanvas, { timestamp });
    const keyFrame = directDelivery.shouldEncodeKeyFrame(
      shouldRequestPeriodicKeyFrame(timestamp, lastKeyFrameTimestampUs)
    );
    try {
      videoEncoder.encode(encodeFrame, { keyFrame });
      encoderMonitor.onSubmitted();
      if (keyFrame) {
        directDelivery.onKeyFrameSubmitted();
        lastKeyFrameTimestampUs = timestamp;
      }
    } finally {
      encodeFrame.close();
    }
  } finally {
    sourceFrame.close();
  }
}

async function stopVideoEncoder({ flush = true } = {}) {
  const encoder = videoEncoder;
  videoEncoder = null;
  if (!encoder) {
    return;
  }

  if (flush && encoder.state === "configured") {
    try {
      await encoder.flush();
    } catch {
      encoderMonitor?.onError();
    }
  }
  if (encoder.state !== "closed") {
    encoder.close();
  }
  activeEncoderGeneration += 1;
  encodeCanvas = null;
  encodeContext = null;
}

function collectTelemetry() {
  if (!monitor && !encoderMonitor && !receiverConnection) {
    return null;
  }
  const directRecoveryElapsedMs =
    directTelemetry.directReconnecting &&
    Number.isFinite(directTelemetry.directRecoveryStartedAt)
      ? Math.max(
        directTelemetry.directRecoveryElapsedMs,
        Date.now() - directTelemetry.directRecoveryStartedAt
      )
      : directTelemetry.directRecoveryElapsedMs;
  return {
    ...(monitor?.sample() ?? {}),
    ...(encoderMonitor?.sample(videoEncoder?.encodeQueueSize ?? 0) ?? {}),
    encoderConfig,
    mediaContract,
    sourceDimensions,
    ...directTelemetry,
    directRecoveryElapsedMs,
    directConnected: receiverConnection?.connected ?? false,
    directBufferedAmount: receiverConnection?.bufferedAmount ?? 0
  };
}

function normalizeEncoderConfig(config) {
  return {
    codec: config.codec,
    width: config.width,
    height: config.height,
    bitrate: config.bitrate,
    framerate: config.framerate,
    hardwareAcceleration: config.hardwareAcceleration,
    latencyMode: config.latencyMode,
    avcFormat: config.avc?.format
  };
}

function handleEncodedChunk(chunk, binding) {
  if (
    !isCurrentEncoderBinding(
      binding,
      activeEncoderGeneration,
      sourceEpoch,
      mediaContract
    )
  ) {
    directTelemetry.directDroppedFrames += 1;
    return;
  }
  encoderMonitor?.onChunk(chunk);
  try {
    directDelivery.deliver(
      chunk,
      receiverConnection,
      binding.sourceEpoch,
      directTelemetry
    );
  } catch (error) {
    directTelemetry.directErrors += 1;
    if (!stopping) {
      running = false;
      void sendToServiceWorker("CAPTURE_FAILURE", {
        error: `发送到平板失败：${normalizeError(error)}`,
        telemetry: collectTelemetry()
      });
      queueMicrotask(() => {
        void stopCapture();
      });
    }
  }
}

async function connectReceiver(directInfo, contract) {
  if (
    !directInfo ||
    !Number.isInteger(directInfo.sourceEpoch) ||
    directInfo.sourceEpoch <= 0
  ) {
    throw new Error("平板连接信息无效");
  }
  sourceEpoch = directInfo.sourceEpoch;
  const normalizedContract = validateMediaContract(contract);
  const connection = new DirectReceiverConnection({
    trustedDevice: directInfo.trustedDevice,
    sourceEpoch,
    mediaContract: normalizedContract
  });
  connection.onControl = applyReceiverControl;
  connection.onClose = (event) => {
    if (receiverConnection !== connection) {
      return;
    }
    directTelemetry.directConnected = false;
    directTelemetry.directLastCloseCode =
      Number.isInteger(event?.code) ? event.code : null;
    directTelemetry.directLastCloseWasClean =
      typeof event?.wasClean === "boolean" ? event.wasClean : null;
    if (!stopping && !reconfiguring && running) {
      directTelemetry.directErrors += 1;
      beginReceiverRecovery(connection);
    }
  };
  receiverConnection = connection;
  try {
    await connection.connect();
  } catch (error) {
    if (receiverConnection === connection) {
      receiverConnection = null;
    }
    throw error;
  }
  directTelemetry.directConnected = true;
}

async function reconfigureForSource(width, height) {
  if (reconfiguring || stopping || !running) {
    return;
  }
  reconfiguring = true;
  directTelemetry.videoReconfigurationState = "reconfiguring";
  directTelemetry.videoReconfigurationCount += 1;
  void publishTelemetry();
  try {
    const nextEpoch = await allocateNextSourceEpoch();
    await closeReceiver();
    await stopVideoEncoder({ flush: false });
    sourceEpoch = nextEpoch;
    captureSettings = videoTrack?.getSettings() ?? captureSettings;
    directDelivery.reset();
    lastKeyFrameTimestampUs = Number.NaN;
    await createVideoEncoder(width, height);
    await connectReceiver({
      trustedDevice,
      sourceEpoch
    }, mediaContract);
    sourceDimensions = { width, height };
    dimensionTracker?.commit(width, height);
    directTelemetry.videoReconfigurationState = "connected";
    directDelivery.requireKeyFrame(directTelemetry);
    void publishTelemetry();
  } catch (error) {
    directTelemetry.videoReconfigurationState = "failed";
    throw new Error(`自动画面尺寸重配失败：${normalizeError(error)}`);
  } finally {
    reconfiguring = false;
  }
}

async function allocateNextSourceEpoch() {
  const response = await chrome.runtime.sendMessage({
    target: "service-worker",
    type: "ALLOCATE_SOURCE_EPOCH"
  });
  if (
    !response?.ok ||
    !Number.isInteger(response.sourceEpoch) ||
    response.sourceEpoch <= sourceEpoch
  ) {
    throw new Error(response?.error || "无法持久化新的来源 epoch");
  }
  return response.sourceEpoch;
}

function beginReceiverRecovery(connection) {
  if (receiverRecoveryPromise !== null || stopping || !running) {
    return;
  }
  const generation = ++receiverRecoveryGeneration;
  const abortController = new AbortController();
  receiverRecoveryAbortController = abortController;
  directTelemetry.directReconnecting = true;
  directTelemetry.directRecoveryState = "reconnecting";
  directTelemetry.directRecoveryStartedAt = Date.now();
  directTelemetry.directRecoveryElapsedMs = 0;
  directTelemetry.directRecoveryCurrentAttempt = 0;
  directTelemetry.directRecoveryTransientFailures = 0;
  directTelemetry.directRecoveryLastOutcome = null;
  void publishTelemetry();
  receiverRecoveryPromise = connection.recover({
    connectTimeoutMs: DIRECT_RECOVERY_CONNECT_TIMEOUT_MS,
    retryDelaysMs: DIRECT_RECOVERY_RETRY_DELAYS_MS,
    signal: abortController.signal,
    shouldContinue: () =>
      generation === receiverRecoveryGeneration &&
      receiverConnection === connection &&
      running &&
      !stopping,
    onAttempt: ({ attempts, elapsedMs }) => {
      if (
        generation !== receiverRecoveryGeneration ||
        receiverConnection !== connection
      ) {
        return;
      }
      directTelemetry.directReconnectAttempts += 1;
      directTelemetry.directRecoveryCurrentAttempt = attempts;
      directTelemetry.directRecoveryElapsedMs = elapsedMs;
    },
    onTransientFailure: ({ elapsedMs }) => {
      if (
        generation !== receiverRecoveryGeneration ||
        receiverConnection !== connection
      ) {
        return;
      }
      directTelemetry.directRecoveryTransientFailures += 1;
      directTelemetry.directRecoveryElapsedMs = elapsedMs;
    }
  }).then(({ elapsedMs }) => {
    if (
      generation !== receiverRecoveryGeneration ||
      receiverConnection !== connection ||
      stopping ||
      !running
    ) {
      void connection.close();
      return;
    }
    directTelemetry.directConnected = true;
    directTelemetry.directReconnecting = false;
    directTelemetry.directReconnects += 1;
    directTelemetry.directRecoveryState = "connected";
    directTelemetry.directRecoveryElapsedMs = elapsedMs;
    directTelemetry.directRecoveryLastDurationMs = elapsedMs;
    directTelemetry.directRecoveryLastOutcome = "recovered";
    directDelivery.requireKeyFrame(directTelemetry);
    void publishTelemetry();
  }).catch((error) => {
    if (
      error instanceof ReceiverRecoveryCancelledError ||
      abortController.signal.aborted
    ) {
      return;
    }
    if (
      generation !== receiverRecoveryGeneration ||
      receiverConnection !== connection ||
      stopping ||
      !running
    ) {
      return;
    }
    directTelemetry.directReconnecting = false;
    directTelemetry.directErrors += 1;
    const elapsedMs = Math.max(
      directTelemetry.directRecoveryElapsedMs,
      Date.now() - directTelemetry.directRecoveryStartedAt
    );
    directTelemetry.directRecoveryState = "failed_permanent";
    directTelemetry.directRecoveryElapsedMs = elapsedMs;
    directTelemetry.directRecoveryLastDurationMs = elapsedMs;
    directTelemetry.directRecoveryLastOutcome = "failed_permanent";
    running = false;
    void sendToServiceWorker("CAPTURE_FAILURE", {
      error: `平板连接恢复失败：${normalizeError(error)}`,
      telemetry: collectTelemetry()
    });
    queueMicrotask(() => {
      void stopCapture();
    });
  }).finally(() => {
    if (generation === receiverRecoveryGeneration) {
      receiverRecoveryPromise = null;
      receiverRecoveryAbortController = null;
    }
  });
}

function applyReceiverControl(message) {
  if (message?.type === "telemetry") {
    applyReceiverTelemetry(message);
    return;
  }
  if (message?.type === "keyframe") {
    directDelivery.requireKeyFrame(directTelemetry);
    directTelemetry.directKeyframeRequests += 1;
    return;
  }
  if (message?.type === "pong") {
    directTelemetry.lastPongAt = Date.now();
    return;
  }
  if (message?.type === "error") {
    directTelemetry.directErrors += 1;
    if (!stopping) {
      running = false;
      void sendToServiceWorker("CAPTURE_FAILURE", {
        error: `平板拒绝接收画面（${String(message.code ?? "unknown").slice(0, 64)}）`,
        telemetry: collectTelemetry()
      });
      queueMicrotask(() => {
        void stopCapture();
      });
    }
  }
}

function applyReceiverTelemetry(message) {
  for (const [source, target, optional = false] of [
    ["receivedFrames", "receiverReceivedFrames"],
    ["receivedBytes", "receiverReceivedBytes"],
    ["receiverDecodedFrames", "receiverDecodedFrames"],
    ["receiverDroppedFrames", "receiverDroppedFrames"],
    ["receiverResyncEvents", "receiverResyncEvents", true],
    ["receiverKeyframeRequests", "receiverKeyframeRequests", true]
  ]) {
    if (optional && message[source] === undefined) {
      continue;
    }
    if (Number.isSafeInteger(message[source]) && message[source] >= 0) {
      directTelemetry[target] = message[source];
    } else {
      directTelemetry.directErrors += 1;
    }
  }
}

async function closeReceiver() {
  receiverRecoveryGeneration += 1;
  receiverRecoveryAbortController?.abort();
  receiverRecoveryAbortController = null;
  const connection = receiverConnection;
  if (!connection) {
    return;
  }
  const recovery = receiverRecoveryPromise;
  receiverRecoveryPromise = null;
  if (directTelemetry.directReconnecting) {
    const elapsedMs = Number.isFinite(directTelemetry.directRecoveryStartedAt)
      ? Math.max(
        directTelemetry.directRecoveryElapsedMs,
        Date.now() - directTelemetry.directRecoveryStartedAt
      )
      : directTelemetry.directRecoveryElapsedMs;
    directTelemetry.directRecoveryState = "cancelled";
    directTelemetry.directRecoveryElapsedMs = elapsedMs;
    directTelemetry.directRecoveryLastDurationMs = elapsedMs;
    directTelemetry.directRecoveryLastOutcome = "cancelled";
  }
  directTelemetry.directReconnecting = false;
  const deadline = performance.now() + 2000;
  while (
    connection.connected &&
    connection.bufferedAmount > 0 &&
    performance.now() < deadline
  ) {
    await delay(20);
  }
  await connection.close();
  if (recovery !== null) {
    try {
      await recovery;
    } catch {
      // Recovery cancellation is expected during an explicit stop.
    }
  }
  receiverConnection = null;
  directTelemetry.directConnected = false;
}

function createDirectTelemetry() {
  return {
    directConnected: false,
    directSentFrames: 0,
    directSentBytes: 0,
    directDroppedFrames: 0,
    directBufferedAmount: 0,
    directErrors: 0,
    directKeyframeRequests: 0,
    directResyncEvents: 0,
    directReconnecting: false,
    directReconnectAttempts: 0,
    directReconnects: 0,
    directRecoveryState: "idle",
    directRecoveryStartedAt: null,
    directRecoveryElapsedMs: 0,
    directRecoveryLastDurationMs: 0,
    directRecoveryTransientFailures: 0,
    directRecoveryCurrentAttempt: 0,
    directRecoveryLastOutcome: null,
    directLastCloseCode: null,
    directLastCloseWasClean: null,
    lastPongAt: null,
    receiverReceivedFrames: 0,
    receiverReceivedBytes: 0,
    receiverDecodedFrames: 0,
    receiverDroppedFrames: 0,
    receiverResyncEvents: 0,
    receiverKeyframeRequests: 0,
    captureSettingsWidth: null,
    captureSettingsHeight: null,
    initialFrameMatchedSettings: null,
    videoReconfigurationCount: 0,
    videoReconfigurationState: "idle"
  };
}

function frameWidth(frame) {
  const width = frame?.displayWidth || frame?.codedWidth;
  if (!Number.isInteger(width) || width < 2) {
    throw new Error("捕获帧宽度无效");
  }
  return width;
}

function frameHeight(frame) {
  const height = frame?.displayHeight || frame?.codedHeight;
  if (!Number.isInteger(height) || height < 2) {
    throw new Error("捕获帧高度无效");
  }
  return height;
}

function positiveIntegerOrNull(value) {
  return Number.isInteger(value) && value > 0 ? value : null;
}


function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function sendToServiceWorker(type, payload = {}) {
  try {
    await chrome.runtime.sendMessage({
      target: "service-worker",
      type,
      ...payload
    });
  } catch {
    // The service worker can be restarting. The next telemetry tick retries.
  }
}

function normalizeError(error) {
  return error instanceof Error ? error.message : String(error);
}
