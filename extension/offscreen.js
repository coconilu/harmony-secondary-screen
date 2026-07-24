import { FrameMonitor } from "./frame-monitor.js";
import { EncoderMonitor } from "./encoder-monitor.js";
import {
  createLocalVideoMessage,
  LOCAL_RELAY_PROTOCOL,
  LOCAL_VIDEO_MAX_PAYLOAD_BYTES
} from "./local-relay-protocol.js";

const TELEMETRY_INTERVAL_MS = 500;
const ENCODE_WIDTH = 1280;
const ENCODE_HEIGHT = 720;
const ENCODE_FRAMERATE = 30;
const ENCODE_BITRATE = 4_000_000;
const MAX_ENCODE_QUEUE_SIZE = 2;
const KEYFRAME_INTERVAL_FRAMES = ENCODE_FRAMERATE * 2;
const LOCAL_RELAY_MAX_BUFFERED_BYTES = 8 * 1024 * 1024;
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
let relaySocket = null;
let relayClosing = false;
let relaySequence = 0;
let relayTelemetry = createRelayTelemetry();
let forceKeyFrame = true;
let running = false;
let stopping = false;

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.target !== "offscreen") {
    return false;
  }

  if (message.type === "START_CAPTURE") {
    startCapture(message.streamId, message.relayInfo)
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

async function startCapture(streamId, relayInfo) {
  if (!streamId) {
    throw new Error("缺少标签页媒体流标识");
  }

  await stopCapture();
  stopping = false;
  relayClosing = false;
  relaySequence = 0;
  relayTelemetry = createRelayTelemetry();
  forceKeyFrame = true;
  await connectLocalRelay(relayInfo);

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
  if (!running && !mediaStream && !frameReader && !relaySocket) {
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
  await closeLocalRelay();
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
  if (!monitor && !encoderMonitor && !relaySocket) {
    return null;
  }
  return {
    ...(monitor?.sample() ?? {}),
    ...(encoderMonitor?.sample(videoEncoder?.encodeQueueSize ?? 0) ?? {}),
    encoderConfig,
    ...relayTelemetry,
    relayConnected: relaySocket?.readyState === WebSocket.OPEN,
    relayBufferedAmount: relaySocket?.bufferedAmount ?? 0
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
    sendChunkToRelay(chunk);
  } catch (error) {
    relayTelemetry.relayErrors += 1;
    if (!stopping) {
      running = false;
      void sendToServiceWorker("CAPTURE_FAILURE", {
        error: `回环 Relay 传输失败：${normalizeError(error)}`,
        telemetry: collectTelemetry()
      });
    }
  }
}

function sendChunkToRelay(chunk) {
  const socket = relaySocket;
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    throw new Error("本地 Relay WebSocket 未连接");
  }
  if (chunk.byteLength > LOCAL_VIDEO_MAX_PAYLOAD_BYTES) {
    throw new Error("单个 H.264 编码块超过 8 MiB");
  }
  if (socket.bufferedAmount >= LOCAL_RELAY_MAX_BUFFERED_BYTES) {
    relayTelemetry.relayDroppedFrames += 1;
    return;
  }

  const message = createLocalVideoMessage(chunk, relaySequence);
  socket.send(message);

  relaySequence = (relaySequence + 1) >>> 0;
  relayTelemetry.relaySentFrames += 1;
  relayTelemetry.relaySentBytes += chunk.byteLength;
}

function connectLocalRelay(relayInfo) {
  if (
    relayInfo?.protocol !== LOCAL_RELAY_PROTOCOL ||
    !Number.isInteger(relayInfo.port) ||
    relayInfo.port < 1 ||
    relayInfo.port > 65535 ||
    typeof relayInfo.token !== "string" ||
    !/^[0-9a-f]{64}$/.test(relayInfo.token)
  ) {
    return Promise.reject(new Error("本地 Relay 启动信息无效"));
  }

  return new Promise((resolve, reject) => {
    let settled = false;
    const socket = new WebSocket(
      `ws://127.0.0.1:${relayInfo.port}/capture`
    );
    socket.binaryType = "arraybuffer";
    relaySocket = socket;

    const timeout = setTimeout(() => {
      if (!settled) {
        settled = true;
        reject(new Error("连接本地 Relay 超时"));
        socket.close();
      }
    }, 5000);

    socket.addEventListener("open", () => {
      socket.send(
        JSON.stringify({
          type: "auth",
          token: relayInfo.token
        })
      );
    });
    socket.addEventListener("message", (event) => {
      if (typeof event.data !== "string") {
        relayTelemetry.relayErrors += 1;
        return;
      }
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        relayTelemetry.relayErrors += 1;
        return;
      }

      if (!settled) {
        if (
          message?.type !== "ready" ||
          message.protocol !== LOCAL_RELAY_PROTOCOL
        ) {
          settled = true;
          clearTimeout(timeout);
          reject(new Error("本地 Relay 鉴权响应无效"));
          socket.close();
          return;
        }
        settled = true;
        clearTimeout(timeout);
        relayTelemetry.relayConnected = true;
        resolve();
        return;
      }

      if (message?.type === "telemetry") {
        applyRelayTelemetry(message);
        return;
      }
      if (message?.type === "keyframe") {
        forceKeyFrame = true;
        return;
      }
      if (message?.type === "error") {
        const code =
          typeof message.code === "string"
            ? message.code.slice(0, 64)
            : "unknown_relay_error";
        const detail =
          typeof message.detail === "string"
            ? message.detail.slice(0, 240)
            : "Relay 未提供错误详情";
        relayTelemetry.relayLastErrorCode = code;
        relayTelemetry.relayLastErrorDetail = detail;
        relayTelemetry.relayErrors += 1;
        if (!stopping) {
          running = false;
          void sendToServiceWorker("CAPTURE_FAILURE", {
            error: `本地 Relay 拒绝数据（${code}）：${detail}`,
            telemetry: collectTelemetry()
          });
        }
      }
    });
    socket.addEventListener("error", () => {
      relayTelemetry.relayErrors += 1;
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        reject(new Error("本地 Relay WebSocket 连接失败"));
      }
    });
    socket.addEventListener("close", () => {
      relayTelemetry.relayConnected = false;
      relaySocket = null;
      if (!settled) {
        settled = true;
        clearTimeout(timeout);
        reject(new Error("本地 Relay 在鉴权前断开"));
        return;
      }
      if (!relayClosing && !stopping && running) {
        relayTelemetry.relayErrors += 1;
        running = false;
        void sendToServiceWorker("CAPTURE_FAILURE", {
          error: "本地 Relay WebSocket 意外断开",
          telemetry: collectTelemetry()
        });
      }
    });
  });
}

function applyRelayTelemetry(message) {
  for (const [source, target] of [
    ["receivedFrames", "relayReceivedFrames"],
    ["receivedBytes", "relayReceivedBytes"],
    ["keyFrames", "relayKeyFrames"],
    ["invalidMessages", "relayInvalidMessages"],
    ["lanSentFrames", "lanSentFrames"],
    ["lanSentBytes", "lanSentBytes"],
    ["lanSentDatagrams", "lanSentDatagrams"],
    ["lanSendErrors", "lanSendErrors"],
    ["receiverDecodedFrames", "receiverDecodedFrames"],
    ["receiverDroppedFrames", "receiverDroppedFrames"]
  ]) {
    if (Number.isSafeInteger(message[source]) && message[source] >= 0) {
      relayTelemetry[target] = message[source];
    } else {
      relayTelemetry.relayErrors += 1;
    }
  }
  if (typeof message.lanConnected === "boolean") {
    relayTelemetry.lanConnected = message.lanConnected;
  }
}

async function closeLocalRelay() {
  const socket = relaySocket;
  if (!socket) {
    return;
  }
  relayClosing = true;
  const deadline = performance.now() + 2000;
  while (
    socket.readyState === WebSocket.OPEN &&
    socket.bufferedAmount > 0 &&
    performance.now() < deadline
  ) {
    await delay(20);
  }

  if (socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ type: "close" }));
    await Promise.race([
      new Promise((resolve) => {
        socket.addEventListener("close", resolve, { once: true });
      }),
      delay(1000)
    ]);
  }
  if (
    socket.readyState === WebSocket.OPEN ||
    socket.readyState === WebSocket.CONNECTING
  ) {
    socket.close();
  }
  relaySocket = null;
  relayTelemetry.relayConnected = false;
}

function createRelayTelemetry() {
  return {
    relayConnected: false,
    relaySentFrames: 0,
    relaySentBytes: 0,
    relayReceivedFrames: 0,
    relayReceivedBytes: 0,
    relayKeyFrames: 0,
    relayInvalidMessages: 0,
    relayDroppedFrames: 0,
    relayBufferedAmount: 0,
    relayErrors: 0,
    relayLastErrorCode: null,
    relayLastErrorDetail: null,
    lanConnected: false,
    lanSentFrames: 0,
    lanSentBytes: 0,
    lanSentDatagrams: 0,
    lanSendErrors: 0,
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
