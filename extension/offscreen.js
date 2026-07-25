import { FrameMonitor } from "./frame-monitor.js";
import { EncoderMonitor } from "./encoder-monitor.js";
import {
  createDirectVideoMessage,
  DIRECT_VIDEO_MAX_PAYLOAD_BYTES
} from "./direct-protocol.js";
import { DirectReceiverConnection } from "./direct-client.js";

const TELEMETRY_INTERVAL_MS = 500;
const ENCODE_WIDTH = 1280;
const ENCODE_HEIGHT = 720;
const ENCODE_FRAMERATE = 30;
const ENCODE_BITRATE = 4_000_000;
const MAX_ENCODE_QUEUE_SIZE = 2;
const KEYFRAME_INTERVAL_FRAMES = ENCODE_FRAMERATE * 2;
const H264_CONFIG = {
  codec: "avc1.42001f",
  width: ENCODE_WIDTH,
  height: ENCODE_HEIGHT,
  bitrate: ENCODE_BITRATE,
  framerate: ENCODE_FRAMERATE,
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
let sourceEpoch = 0;
let directSequence = 0;
let directTelemetry = createDirectTelemetry();
let forceKeyFrame = true;
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
  forceKeyFrame = true;
  await connectReceiver(directInfo);

  mediaStream = await navigator.mediaDevices.getUserMedia({
    audio: false,
    video: {
      mandatory: {
        chromeMediaSource: "tab",
        chromeMediaSourceId: streamId
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
      "当前 Edge 不支持 1280×720 @ 30 fps 的 H.264 Annex-B 编码"
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

  encodeCanvas = new OffscreenCanvas(ENCODE_WIDTH, ENCODE_HEIGHT);
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
      ENCODE_WIDTH / sourceWidth,
      ENCODE_HEIGHT / sourceHeight
    );
    const targetWidth = Math.max(1, Math.round(sourceWidth * scale));
    const targetHeight = Math.max(1, Math.round(sourceHeight * scale));
    const targetX = Math.floor((ENCODE_WIDTH - targetWidth) / 2);
    const targetY = Math.floor((ENCODE_HEIGHT - targetHeight) / 2);

    encodeContext.fillStyle = "#000";
    encodeContext.fillRect(0, 0, ENCODE_WIDTH, ENCODE_HEIGHT);
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
    const keyFrame =
      forceKeyFrame ||
      encoderMonitor.submittedFrames % KEYFRAME_INTERVAL_FRAMES === 0;
    try {
      videoEncoder.encode(encodeFrame, { keyFrame });
      encoderMonitor.onSubmitted();
      if (keyFrame) {
        forceKeyFrame = false;
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
  return {
    ...(monitor?.sample() ?? {}),
    ...(encoderMonitor?.sample(videoEncoder?.encodeQueueSize ?? 0) ?? {}),
    encoderConfig,
    ...directTelemetry,
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
  try {
    sendChunkToReceiver(chunk);
  } catch (error) {
    directTelemetry.directErrors += 1;
    if (!stopping) {
      running = false;
      void sendToServiceWorker("CAPTURE_FAILURE", {
        error: `Receiver 直连传输失败：${normalizeError(error)}`,
        telemetry: collectTelemetry()
      });
    }
  }
}

function sendChunkToReceiver(chunk) {
  const connection = receiverConnection;
  if (!connection?.connected) {
    throw new Error("Receiver WebSocket 未连接");
  }
  if (chunk.byteLength > DIRECT_VIDEO_MAX_PAYLOAD_BYTES) {
    throw new Error("单个 H.264 编码块超过 8 MiB");
  }
  const message = createDirectVideoMessage(chunk, sourceEpoch, directSequence);
  if (!connection.sendVideo(message)) {
    directTelemetry.directDroppedFrames += 1;
    return;
  }
  directSequence = (directSequence + 1) >>> 0;
  directTelemetry.directSentFrames += 1;
  directTelemetry.directSentBytes += chunk.byteLength;
}

async function connectReceiver(directInfo) {
  if (
    !directInfo ||
    !Number.isInteger(directInfo.sourceEpoch) ||
    directInfo.sourceEpoch <= 0
  ) {
    throw new Error("直连 Receiver 参数无效");
  }
  sourceEpoch = directInfo.sourceEpoch;
  const connection = new DirectReceiverConnection({
    trustedDevice: directInfo.trustedDevice,
    sourceEpoch
  });
  connection.onControl = applyReceiverControl;
  connection.onClose = () => {
    directTelemetry.directConnected = false;
    if (!stopping && running) {
      directTelemetry.directErrors += 1;
      running = false;
      void sendToServiceWorker("CAPTURE_FAILURE", {
        error: "Receiver 直连 WebSocket 意外断开",
        telemetry: collectTelemetry()
      });
    }
  };
  await connection.connect();
  receiverConnection = connection;
  directTelemetry.directConnected = true;
}

function applyReceiverControl(message) {
  if (message?.type === "telemetry") {
    applyReceiverTelemetry(message);
    return;
  }
  if (message?.type === "keyframe") {
    forceKeyFrame = true;
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
        error: `Receiver 拒绝数据（${String(message.code ?? "unknown").slice(0, 64)}）`,
        telemetry: collectTelemetry()
      });
    }
  }
}

function applyReceiverTelemetry(message) {
  for (const [source, target] of [
    ["receivedFrames", "receiverReceivedFrames"],
    ["receivedBytes", "receiverReceivedBytes"],
    ["receiverDecodedFrames", "receiverDecodedFrames"],
    ["receiverDroppedFrames", "receiverDroppedFrames"]
  ]) {
    if (Number.isSafeInteger(message[source]) && message[source] >= 0) {
      directTelemetry[target] = message[source];
    } else {
      directTelemetry.directErrors += 1;
    }
  }
}

async function closeReceiver() {
  const connection = receiverConnection;
  if (!connection) {
    return;
  }
  const deadline = performance.now() + 2000;
  while (
    connection.connected &&
    connection.bufferedAmount > 0 &&
    performance.now() < deadline
  ) {
    await delay(20);
  }
  await connection.close();
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
    lastPongAt: null,
    receiverReceivedFrames: 0,
    receiverReceivedBytes: 0,
    receiverDecodedFrames: 0,
    receiverDroppedFrames: 0
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
