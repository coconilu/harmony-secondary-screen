import { FrameMonitor } from "./frame-monitor.js";
import { EncoderMonitor } from "./encoder-monitor.js";
import {
  createDirectVideoMessage,
  DIRECT_VIDEO_BITRATE,
  DIRECT_VIDEO_CODEC,
  DIRECT_VIDEO_FRAMERATE,
  DIRECT_VIDEO_HEIGHT,
  DIRECT_VIDEO_MAX_PAYLOAD_BYTES,
  DIRECT_VIDEO_WIDTH
} from "./direct-protocol.js";
import {
  DIRECT_RECOVERY_CONNECT_TIMEOUT_MS,
  DIRECT_RECOVERY_RETRY_DELAYS_MS,
  DirectReceiverConnection,
  ReceiverRecoveryCancelledError
} from "./direct-client.js";
import { DirectResyncPolicy } from "./direct-resync-policy.js";
import { shouldRequestPeriodicKeyFrame } from "./keyframe-policy.js";

const TELEMETRY_INTERVAL_MS = 500;
const MAX_ENCODE_QUEUE_SIZE = 2;
const H264_CONFIG = {
  codec: DIRECT_VIDEO_CODEC,
  width: DIRECT_VIDEO_WIDTH,
  height: DIRECT_VIDEO_HEIGHT,
  bitrate: DIRECT_VIDEO_BITRATE,
  framerate: DIRECT_VIDEO_FRAMERATE,
  hardwareAcceleration: "prefer-hardware",
  latencyMode: "realtime",
  alpha: "discard",
  avc: {
    format: "annexb"
  }
};

let mediaStream = null;
let videoTrack = null;
let frameReader = null;
let frameLoopPromise = null;
let telemetryTimer = null;
let monitor = null;
let encoderMonitor = null;
let videoEncoder = null;
let encoderConfig = null;
let encodeCanvas = null;
let encodeContext = null;
let receiverConnection = null;
let receiverRecoveryGeneration = 0;
let receiverRecoveryPromise = null;
let receiverRecoveryAbortController = null;
let sourceEpoch = 0;
let directSequence = 0;
let directTelemetry = createDirectTelemetry();
const directResyncPolicy = new DirectResyncPolicy();
let lastKeyFrameTimestampUs = Number.NaN;
let running = false;
let stopping = false;

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
  stopping = false;
  directSequence = 0;
  directTelemetry = createDirectTelemetry();
  directResyncPolicy.reset();
  lastKeyFrameTimestampUs = Number.NaN;
  await connectReceiver(directInfo);

  mediaStream = await navigator.mediaDevices.getUserMedia({
    audio: false,
    video: {
      mandatory: {
        chromeMediaSource: "tab",
        chromeMediaSourceId: streamId,
        maxFrameRate: DIRECT_VIDEO_FRAMERATE
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
      frameRate: DIRECT_VIDEO_FRAMERATE
    });
  } catch {
    // 部分 Edge/Chromium 版本只识别上面的 legacy maxFrameRate。
    // 监控面板仍显示实测帧率，不把配置目标误报成已达到的帧率。
  }
  videoTrack.addEventListener("ended", handleTrackEnded, { once: true });

  await createVideoEncoder();
  const processor = new MediaStreamTrackProcessor({ track: videoTrack });
  frameReader = processor.readable.getReader();
  monitor = new FrameMonitor();
  encoderMonitor = new EncoderMonitor();
  running = true;

  frameLoopPromise = consumeFrames();
  telemetryTimer = setInterval(publishTelemetry, TELEMETRY_INTERVAL_MS);
  const telemetry = collectTelemetry();
  return telemetry;
}

async function consumeFrames() {
  try {
    while (running && frameReader) {
      const { value: frame, done } = await frameReader.read();
      if (done || !frame) {
        break;
      }

      monitor?.onFrame();
      encodeFrame(frame);
    }
  } catch (error) {
    if (!stopping) {
      await sendToServiceWorker("CAPTURE_FAILURE", {
        error: normalizeError(error),
        telemetry: collectTelemetry()
      });
    }
  } finally {
    if (!stopping && running) {
      running = false;
      await sendToServiceWorker("CAPTURE_ENDED", {
        reason: "video_track_ended",
        telemetry: collectTelemetry()
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
  if (!running && !mediaStream && !frameReader && !receiverConnection) {
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

  try {
    await frameLoopPromise;
  } catch {
    // consumeFrames reports unexpected failures itself.
  }
  frameLoopPromise = null;

  await stopVideoEncoder();
  await closeReceiver();
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
}

async function createVideoEncoder() {
  const support = await VideoEncoder.isConfigSupported(H264_CONFIG);
  if (!support.supported) {
    throw new Error(
      `当前 Edge 不支持 ${DIRECT_VIDEO_WIDTH}×${DIRECT_VIDEO_HEIGHT} @ ` +
      `${DIRECT_VIDEO_FRAMERATE} fps 的 H.264 编码`
    );
  }

  encoderConfig = normalizeEncoderConfig(support.config ?? H264_CONFIG);
  videoEncoder = new VideoEncoder({
    output: (chunk) => {
      handleEncodedChunk(chunk);
    },
    error: (error) => {
      encoderMonitor?.onError();
      if (!stopping) {
        running = false;
        void sendToServiceWorker("CAPTURE_FAILURE", {
          error: `H.264 编码失败：${normalizeError(error)}`,
          telemetry: collectTelemetry()
        });
      }
    }
  });
  videoEncoder.configure(support.config ?? H264_CONFIG);

  encodeCanvas = new OffscreenCanvas(DIRECT_VIDEO_WIDTH, DIRECT_VIDEO_HEIGHT);
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

    const sourceWidth = sourceFrame.displayWidth || sourceFrame.codedWidth;
    const sourceHeight = sourceFrame.displayHeight || sourceFrame.codedHeight;
    if (!sourceWidth || !sourceHeight) {
      throw new Error("捕获帧尺寸无效");
    }

    const scale = Math.min(
      DIRECT_VIDEO_WIDTH / sourceWidth,
      DIRECT_VIDEO_HEIGHT / sourceHeight
    );
    const targetWidth = Math.max(1, Math.round(sourceWidth * scale));
    const targetHeight = Math.max(1, Math.round(sourceHeight * scale));
    const targetX = Math.floor((DIRECT_VIDEO_WIDTH - targetWidth) / 2);
    const targetY = Math.floor((DIRECT_VIDEO_HEIGHT - targetHeight) / 2);

    encodeContext.fillStyle = "#000";
    encodeContext.fillRect(
      0,
      0,
      DIRECT_VIDEO_WIDTH,
      DIRECT_VIDEO_HEIGHT
    );
    encodeContext.drawImage(
      sourceFrame,
      targetX,
      targetY,
      targetWidth,
      targetHeight
    );

    const timestamp =
      sourceFrame.timestamp ?? Math.round(performance.now() * 1000);
    const encodeFrame = new VideoFrame(encodeCanvas, { timestamp });
    const keyFrame = directResyncPolicy.shouldEncodeKeyFrame(
      shouldRequestPeriodicKeyFrame(timestamp, lastKeyFrameTimestampUs)
    );
    try {
      videoEncoder.encode(encodeFrame, { keyFrame });
      encoderMonitor.onSubmitted();
      if (keyFrame) {
        directResyncPolicy.onKeyFrameSubmitted();
        lastKeyFrameTimestampUs = timestamp;
      }
    } finally {
      encodeFrame.close();
    }
  } finally {
    sourceFrame.close();
  }
}

async function stopVideoEncoder() {
  const encoder = videoEncoder;
  videoEncoder = null;
  if (!encoder) {
    return;
  }

  if (encoder.state === "configured") {
    try {
      await encoder.flush();
    } catch {
      encoderMonitor?.onError();
    }
  }
  if (encoder.state !== "closed") {
    encoder.close();
  }
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

function handleEncodedChunk(chunk) {
  encoderMonitor?.onChunk(chunk);
  if (!receiverConnection?.connected) {
    dropDirectFrameForResync();
    return;
  }
  if (!directResyncPolicy.canDeliverEncodedChunk(chunk.type)) {
    dropDirectFrameForResync();
    return;
  }
  try {
    sendChunkToReceiver(chunk);
  } catch (error) {
    directTelemetry.directErrors += 1;
    if (!stopping) {
      running = false;
      void sendToServiceWorker("CAPTURE_FAILURE", {
        error: `发送到平板失败：${normalizeError(error)}`,
        telemetry: collectTelemetry()
      });
    }
  }
}

function sendChunkToReceiver(chunk) {
  const connection = receiverConnection;
  if (chunk.byteLength > DIRECT_VIDEO_MAX_PAYLOAD_BYTES) {
    throw new Error("单个 H.264 编码块超过 8 MiB");
  }
  if (!connection?.connected) {
    dropDirectFrameForResync();
    return;
  }
  const message = createDirectVideoMessage(chunk, sourceEpoch, directSequence);
  if (!connection.sendVideo(message)) {
    dropDirectFrameForResync();
    return;
  }
  directSequence = (directSequence + 1) >>> 0;
  directResyncPolicy.onEncodedChunkDelivered(chunk.type);
  directTelemetry.directSentFrames += 1;
  directTelemetry.directSentBytes += chunk.byteLength;
}

async function connectReceiver(directInfo) {
  if (
    !directInfo ||
    !Number.isInteger(directInfo.sourceEpoch) ||
    directInfo.sourceEpoch <= 0
  ) {
    throw new Error("平板连接信息无效");
  }
  sourceEpoch = directInfo.sourceEpoch;
  const connection = new DirectReceiverConnection({
    trustedDevice: directInfo.trustedDevice,
    sourceEpoch
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
    if (!stopping && running) {
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
    requireDirectKeyFrame();
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
    requireDirectKeyFrame();
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

function requireDirectKeyFrame() {
  if (directResyncPolicy.requireKeyFrame()) {
    directTelemetry.directResyncEvents += 1;
  }
}

function dropDirectFrameForResync() {
  directTelemetry.directDroppedFrames += 1;
  requireDirectKeyFrame();
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
    receiverKeyframeRequests: 0
  };
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
