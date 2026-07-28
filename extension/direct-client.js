import {
  createDirectWebSocketUrl,
  DIRECT_MAX_BUFFERED_BYTES,
  DIRECT_PROTOCOL,
  DIRECT_VIDEO_FRAMERATE,
  DIRECT_VIDEO_HEIGHT,
  DIRECT_VIDEO_WIDTH,
  validateTrustedDevice
} from "./direct-protocol.js";
import {
  beginDirectTransportObservation,
  DirectTransportError,
  finishDirectTransportObservation
} from "./direct-network-diagnostics.js";

const CONNECT_TIMEOUT_MS = 5_000;
export const DIRECT_RECOVERY_CONNECT_TIMEOUT_MS = 1_500;
export const DIRECT_RECOVERY_MAX_RETRY_DELAY_MS = 2_000;
export const DIRECT_RECOVERY_RETRY_DELAYS_MS = Object.freeze([
  0,
  250,
  500,
  1_000,
  2_000
]);

export async function pairReceiver({
  host,
  authorization,
  senderId,
  socketFactory = (url) => new WebSocket(url),
  beginTransportObservation = beginDirectTransportObservation,
  finishTransportObservation = finishDirectTransportObservation
}) {
  const url = createDirectWebSocketUrl(host);
  const observation = await beginTransportObservation(url);
  const socket = socketFactory(url);
  try {
    const response = await openAndExchange(socket, {
      type: "pair",
      protocol: DIRECT_PROTOCOL,
      sessionId: authorization.sessionId,
      token: authorization.token,
      senderId
    }, "paired", CONNECT_TIMEOUT_MS, undefined, {
      host,
      observation,
      finish: finishTransportObservation
    });
    return validateTrustedDevice({
      senderId,
      deviceId: response.deviceId,
      credential: response.credential,
      host,
      pairedAt: Date.now()
    });
  } finally {
    closeSocket(socket);
  }
}

export class DirectReceiverConnection {
  constructor({
    trustedDevice,
    sourceEpoch,
    socketFactory = (url) => new WebSocket(url),
    heartbeatIntervalMs = 5_000,
    connectTimeoutMs = CONNECT_TIMEOUT_MS,
    beginTransportObservation = beginDirectTransportObservation,
    finishTransportObservation = finishDirectTransportObservation
  }) {
    this.trustedDevice = validateTrustedDevice(trustedDevice);
    this.sourceEpoch = sourceEpoch;
    this.socketFactory = socketFactory;
    this.heartbeatIntervalMs = heartbeatIntervalMs;
    this.connectTimeoutMs = connectTimeoutMs;
    this.beginTransportObservation = beginTransportObservation;
    this.finishTransportObservation = finishTransportObservation;
    this.socket = null;
    this.authenticated = false;
    this.onControl = () => {};
    this.onClose = () => {};
    this.heartbeatTimer = null;
  }

  async connect({ timeoutMs = this.connectTimeoutMs, signal } = {}) {
    throwIfRecoveryCancelled(signal);
    if (this.socket !== null) {
      throw new Error("平板连接已存在");
    }
    const url = createDirectWebSocketUrl(this.trustedDevice.host);
    const observation = await this.beginTransportObservation(url);
    const socket = this.socketFactory(url);
    this.socket = socket;
    this.authenticated = false;
    let response;
    try {
      response = await openAndExchange(socket, {
        type: "auth",
        protocol: DIRECT_PROTOCOL,
        senderId: this.trustedDevice.senderId,
        deviceId: this.trustedDevice.deviceId,
        credential: this.trustedDevice.credential,
        sourceEpoch: this.sourceEpoch,
        codec: "video/avc",
        avcFormat: "annexb",
        width: DIRECT_VIDEO_WIDTH,
        height: DIRECT_VIDEO_HEIGHT,
        fps: DIRECT_VIDEO_FRAMERATE
      }, "ready", timeoutMs, signal, {
        host: this.trustedDevice.host,
        observation,
        finish: this.finishTransportObservation
      });
    } catch (error) {
      if (this.socket === socket) {
        this.socket = null;
      }
      this.authenticated = false;
      closeSocket(socket);
      throw error;
    }
    if (response.sourceEpoch !== this.sourceEpoch) {
      socket.close(1008, "epoch_mismatch");
      if (this.socket === socket) {
        this.socket = null;
      }
      this.authenticated = false;
      throw new ReceiverProtocolError("epoch_stale");
    }
    socket.addEventListener("message", (event) => {
      if (typeof event.data !== "string") {
        return;
      }
      try {
        const message = JSON.parse(event.data);
        if (message?.protocol === DIRECT_PROTOCOL) {
          this.onControl(message);
        }
      } catch {
        // Invalid control messages never enter logs or persistent state.
      }
    });
    socket.addEventListener("close", (event) => {
      this.stopHeartbeat();
      if (this.socket === socket) {
        this.socket = null;
        this.authenticated = false;
      }
      this.onClose(event);
    });
    this.heartbeatTimer = setInterval(() => {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
          type: "ping",
          protocol: DIRECT_PROTOCOL,
          at: Date.now()
        }));
      }
    }, this.heartbeatIntervalMs);
    this.authenticated = true;
    return response;
  }

  async recover({
    connectTimeoutMs = DIRECT_RECOVERY_CONNECT_TIMEOUT_MS,
    retryDelaysMs = DIRECT_RECOVERY_RETRY_DELAYS_MS,
    shouldContinue = () => true,
    onAttempt = () => {},
    onTransientFailure = () => {},
    signal,
    now = () => Date.now(),
    delayFn = delay,
    connectAttempt = (options) => this.connect(options)
  } = {}) {
    const startedAt = now();
    const delays = normalizeRetryDelays(retryDelaysMs);
    let attempts = 0;
    while (shouldContinue() && !signal?.aborted) {
      const delayIndex = Math.min(attempts, delays.length - 1);
      const retryDelayMs = delays[delayIndex];
      if (retryDelayMs > 0) {
        await delayFn(retryDelayMs, signal);
      }
      throwIfRecoveryCancelled(signal, shouldContinue);
      attempts += 1;
      onAttempt({
        attempts,
        elapsedMs: Math.max(0, now() - startedAt),
        retryDelayMs
      });
      try {
        const response = await connectAttempt({
          timeoutMs: connectTimeoutMs,
          signal
        });
        return {
          response,
          attempts,
          elapsedMs: Math.max(0, now() - startedAt)
        };
      } catch (error) {
        if (
          error instanceof ReceiverRecoveryCancelledError ||
          signal?.aborted ||
          !shouldContinue()
        ) {
          throw new ReceiverRecoveryCancelledError();
        }
        if (error?.recoverable === false) {
          throw error;
        }
        onTransientFailure({
          attempts,
          elapsedMs: Math.max(0, now() - startedAt),
          retryDelayMs
        });
      }
    }
    throw new ReceiverRecoveryCancelledError();
  }

  get connected() {
    return this.authenticated && this.socket?.readyState === WebSocket.OPEN;
  }

  get bufferedAmount() {
    return this.socket?.bufferedAmount ?? 0;
  }

  sendVideo(message) {
    if (!this.connected) {
      throw new Error("平板连接尚未建立");
    }
    if (!(message instanceof ArrayBuffer)) {
      throw new Error("视频消息必须是 ArrayBuffer");
    }
    if (this.bufferedAmount >= DIRECT_MAX_BUFFERED_BYTES) {
      return false;
    }
    this.socket.send(message);
    return true;
  }

  async close() {
    const socket = this.socket;
    this.stopHeartbeat();
    this.authenticated = false;
    if (!socket) {
      return;
    }
    if (socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({
        type: "close",
        protocol: DIRECT_PROTOCOL
      }));
      socket.close(1000, "capture_stopped");
    } else if (socket.readyState === WebSocket.CONNECTING) {
      socket.close();
    }
    this.socket = null;
  }

  stopHeartbeat() {
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }
}

function openAndExchange(
  socket,
  request,
  expectedType,
  timeoutMs = CONNECT_TIMEOUT_MS,
  signal,
  transport
) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let timeout = null;
    let opened = false;
    let transportVerdict = null;
    const cleanup = () => {
      if (timeout !== null) {
        clearTimeout(timeout);
        timeout = null;
      }
      socket.removeEventListener("open", handleOpen);
      socket.removeEventListener("message", handleMessage);
      socket.removeEventListener("error", handleError);
      socket.removeEventListener("close", handleClose);
      signal?.removeEventListener("abort", handleAbort);
    };
    const finish = (error, value) => {
      if (settled) {
        return;
      }
      settled = true;
      cleanup();
      if (error) {
        reject(error);
      } else {
        resolve(value);
      }
    };
    const assessTransport = async (socketOutcome) => {
      if (transportVerdict === null) {
        transportVerdict = Promise.resolve(transport?.finish(
          transport.observation,
          transport.host,
          socketOutcome
        ));
      }
      return transportVerdict;
    };
    const handleOpen = async () => {
      try {
        const verdict = await assessTransport("open");
        if (verdict && !verdict.allowAuthentication) {
          finish(new DirectTransportError(verdict));
          closeSocket(socket);
          return;
        }
        opened = true;
        socket.send(JSON.stringify(request));
      } catch (error) {
        finish(error);
        closeSocket(socket);
      }
    };
    const handleMessage = (event) => {
      if (typeof event.data !== "string") {
        finish(new ReceiverProtocolError("invalid_response"));
        return;
      }
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        finish(new ReceiverProtocolError("invalid_response"));
        return;
      }
      if (message?.type === "error") {
        finish(new ReceiverProtocolError(message.code));
        return;
      }
      if (
        message?.type !== expectedType ||
        message.protocol !== DIRECT_PROTOCOL
      ) {
        finish(new ReceiverProtocolError("protocol_mismatch"));
        return;
      }
      finish(null, message);
    };
    const handleError = async () => {
      try {
        const verdict = await assessTransport("error");
        finish(verdict
          ? new DirectTransportError(verdict)
          : new Error("Receiver WebSocket 不可达"));
      } catch (error) {
        finish(error);
      }
    };
    const handleClose = async () => {
      if (opened) {
        finish(new Error("Receiver WebSocket 已建立，但在鉴权完成前断开"));
        return;
      }
      await handleError();
    };
    const handleAbort = () => {
      finish(new ReceiverRecoveryCancelledError());
      closeSocket(socket);
    };

    timeout = setTimeout(() => {
      void (async () => {
        if (opened) {
          finish(new ReceiverProtocolError("handshake_timeout"));
        } else {
          try {
            const verdict = await assessTransport("timeout");
            finish(verdict
              ? new DirectTransportError(verdict)
              : new Error("连接 Receiver 超时"));
          } catch (error) {
            finish(error);
          }
        }
        closeSocket(socket);
      })();
    }, timeoutMs);
    if (signal?.aborted) {
      handleAbort();
      return;
    }
    signal?.addEventListener("abort", handleAbort, { once: true });
    socket.binaryType = "arraybuffer";
    socket.addEventListener("open", handleOpen, { once: true });
    socket.addEventListener("message", handleMessage);
    socket.addEventListener("error", handleError, { once: true });
    socket.addEventListener("close", handleClose, { once: true });
  });
}

function closeSocket(socket) {
  if (
    socket.readyState === WebSocket.OPEN ||
    socket.readyState === WebSocket.CONNECTING
  ) {
    socket.close();
  }
}

function delay(milliseconds, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new ReceiverRecoveryCancelledError());
      return;
    }
    let timeout = null;
    const handleAbort = () => {
      if (timeout !== null) {
        clearTimeout(timeout);
      }
      signal?.removeEventListener("abort", handleAbort);
      reject(new ReceiverRecoveryCancelledError());
    };
    timeout = setTimeout(() => {
      signal?.removeEventListener("abort", handleAbort);
      resolve();
    }, milliseconds);
    signal?.addEventListener("abort", handleAbort, { once: true });
  });
}

function normalizeRetryDelays(retryDelaysMs) {
  const values = Array.isArray(retryDelaysMs) && retryDelaysMs.length > 0
    ? retryDelaysMs
    : [0];
  return values.map((value) => {
    const milliseconds = Number.isFinite(value) ? Math.max(0, value) : 0;
    return Math.min(milliseconds, DIRECT_RECOVERY_MAX_RETRY_DELAY_MS);
  });
}

function throwIfRecoveryCancelled(signal, shouldContinue = () => true) {
  if (signal?.aborted || !shouldContinue()) {
    throw new ReceiverRecoveryCancelledError();
  }
}

export class ReceiverRecoveryCancelledError extends Error {
  constructor() {
    super("平板连接恢复已取消");
    this.name = "ReceiverRecoveryCancelledError";
    this.recoverable = false;
  }
}

class ReceiverProtocolError extends Error {
  constructor(code) {
    super(describeReceiverError(code));
    this.name = "ReceiverProtocolError";
    this.code = String(code ?? "unknown").slice(0, 64);
    this.recoverable = false;
  }
}

function describeReceiverError(code) {
  return {
    authorization_expired: "二维码或短码已经过期，请重新生成",
    authorization_replayed: "该一次性授权已经使用，请重新生成",
    identity_mismatch: "平板身份不匹配，请忘记设备后重新配对",
    not_paired: "该电脑尚未获得平板授权",
    pairing_failed: "一次性授权不匹配",
    protocol_mismatch: "电脑扩展与平板应用版本不兼容，请同时更新后重试",
    codec_unsupported: "当前平板无法播放这组视频参数",
    epoch_stale: "平板已切换到更新的页面来源",
    invalid_response: "平板返回了无效的连接响应",
    handshake_timeout: "WebSocket 已建立，但 Receiver 鉴权响应超时"
  }[code] ?? `平板拒绝连接（${String(code ?? "unknown").slice(0, 64)}）`;
}
