import {
  createDirectWebSocketUrl,
  computeAuthProof,
  computePairProof,
  constantTimeEqualProof,
  createProofNonce,
  DEFAULT_RECEIVER_HOST,
  DIRECT_MAX_BUFFERED_BYTES,
  DIRECT_PROTOCOL,
  DIRECT_VIDEO_FRAMERATE,
  DIRECT_VIDEO_HEIGHT,
  DIRECT_VIDEO_WIDTH,
  isProofNonce,
  normalizeReceiverHost,
  validateTrustedDevice
} from "./direct-protocol.js";

const CONNECT_TIMEOUT_MS = 5_000;
const DIRECT_MAX_CONTROL_TEXT_BYTES = 2_048;
const controlTextEncoder = new TextEncoder();
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
  randomBytes = crypto.getRandomValues.bind(crypto)
}) {
  const normalizedHost = normalizeReceiverHost(host);
  const url = createDirectWebSocketUrl(normalizedHost);
  const automaticAddress = normalizedHost === DEFAULT_RECEIVER_HOST;
  const nonce = automaticAddress ? createProofNonce(randomBytes) : null;
  const socket = socketFactory(url);
  try {
    const common = {
      expectedType: "paired",
      connectionError: connectionErrorForHost(normalizedHost)
    };
    const response = automaticAddress
      ? await openProofExchange(socket, {
        ...common,
        challenge: {
          type: "pair_challenge",
          protocol: DIRECT_PROTOCOL,
          mode: "pair",
          sessionId: authorization.sessionId,
          senderId,
          nonce
        },
        proofType: "pair_proof",
        async buildSecretRequest(proof) {
          if (
            proof.mode !== "pair" ||
            proof.proofMode !== "qr" ||
            proof.sessionId !== authorization.sessionId ||
            proof.senderId !== senderId ||
            proof.nonce !== nonce ||
            !/^[0-9a-f]{32}$/.test(proof.deviceId ?? "") ||
            !/^[0-9a-f]{64}$/.test(proof.proof ?? "")
          ) {
            throw new ReceiverProtocolError("challenge_response_invalid");
          }
          const expectedProof = await computePairProof({
            token: authorization.token,
            proofMode: "qr",
            sessionId: authorization.sessionId,
            senderId,
            nonce,
            deviceId: proof.deviceId
          });
          if (!constantTimeEqualProof(proof.proof, expectedProof)) {
            throw new ReceiverProtocolError("challenge_proof_invalid");
          }
          return {
            type: "pair",
            protocol: DIRECT_PROTOCOL,
            mode: "pair",
            sessionId: authorization.sessionId,
            token: authorization.token,
            senderId,
            nonce
          };
        },
        validateFinal(responseValue, proof) {
          if (responseValue.deviceId !== proof.deviceId) {
            throw new ReceiverProtocolError("challenge_response_invalid");
          }
        }
      })
      : await openProofExchange(socket, {
        ...common,
        challenge: {
          type: "pair_manual_ipv4",
          protocol: DIRECT_PROTOCOL,
          mode: "pair_manual_ipv4",
          sessionId: authorization.sessionId,
          token: authorization.token,
          senderId
        },
        skipProof: true
      });
    return validateTrustedDevice({
      senderId,
      deviceId: response.deviceId,
      credential: response.credential,
      host: normalizedHost,
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
    randomBytes = crypto.getRandomValues.bind(crypto)
  }) {
    this.trustedDevice = validateTrustedDevice(trustedDevice);
    this.sourceEpoch = sourceEpoch;
    this.socketFactory = socketFactory;
    this.heartbeatIntervalMs = heartbeatIntervalMs;
    this.connectTimeoutMs = connectTimeoutMs;
    this.randomBytes = randomBytes;
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
    const nonce = createProofNonce(this.randomBytes);
    const trustedDevice = this.trustedDevice;
    const socket = this.socketFactory(url);
    this.socket = socket;
    this.authenticated = false;
    let response;
    try {
      const challenge = {
        type: "auth_challenge",
        protocol: DIRECT_PROTOCOL,
        mode: "auth",
        senderId: this.trustedDevice.senderId,
        deviceId: this.trustedDevice.deviceId,
        sourceEpoch: this.sourceEpoch,
        nonce,
        codec: "video/avc",
        avcFormat: "annexb",
        width: DIRECT_VIDEO_WIDTH,
        height: DIRECT_VIDEO_HEIGHT,
        fps: DIRECT_VIDEO_FRAMERATE
      };
      response = await openProofExchange(socket, {
        challenge,
        proofType: "auth_proof",
        expectedType: "ready",
        connectionError: connectionErrorForHost(this.trustedDevice.host),
        timeoutMs,
        signal,
        async buildSecretRequest(proof) {
          if (
            proof.mode !== "auth" ||
            proof.senderId !== challenge.senderId ||
            proof.deviceId !== challenge.deviceId ||
            proof.sourceEpoch !== challenge.sourceEpoch ||
            proof.nonce !== nonce ||
            !isProofNonce(proof.nonce) ||
            !/^[0-9a-f]{64}$/.test(proof.proof ?? "")
          ) {
            throw new ReceiverProtocolError("challenge_response_invalid");
          }
          const expectedProof = await computeAuthProof({
            credential: trustedDevice.credential,
            ...challenge
          });
          if (!constantTimeEqualProof(proof.proof, expectedProof)) {
            throw new ReceiverProtocolError("challenge_proof_invalid");
          }
          return {
            type: "auth",
            protocol: DIRECT_PROTOCOL,
            mode: "auth",
            senderId: challenge.senderId,
            deviceId: challenge.deviceId,
            credential: trustedDevice.credential,
            sourceEpoch: challenge.sourceEpoch,
            nonce,
            codec: challenge.codec,
            avcFormat: challenge.avcFormat,
            width: challenge.width,
            height: challenge.height,
            fps: challenge.fps
          };
        }
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

function openProofExchange(socket, {
  challenge,
  proofType,
  expectedType,
  buildSecretRequest,
  skipProof = false,
  validateFinal = () => {},
  connectionError = {
    code: "receiver_connection_failed",
    message: "无法连接 Receiver，请确认平板已开始接收"
  },
  timeoutMs = CONNECT_TIMEOUT_MS,
  signal
}) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let timeout = null;
    let stage = "opening";
    let proofMessage = null;
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
    const handleOpen = () => {
      try {
        stage = skipProof ? "awaiting_result" : "awaiting_proof";
        socket.send(JSON.stringify(challenge));
      } catch (error) {
        finish(error);
        closeSocket(socket);
      }
    };
    const handleMessage = async (event) => {
      if (settled) {
        return;
      }
      if (!["awaiting_proof", "awaiting_result"].includes(stage)) {
        finish(new ReceiverProtocolError("unsolicited_response"));
        closeSocket(socket);
        return;
      }
      if (typeof event.data !== "string") {
        finish(new ReceiverProtocolError("invalid_response"));
        return;
      }
      if (controlMessageExceedsLimit(event.data)) {
        finish(new ReceiverProtocolError("control_message_too_large"));
        closeSocket(socket);
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
      if (message?.protocol !== DIRECT_PROTOCOL) {
        finish(new ReceiverProtocolError("protocol_mismatch"));
        return;
      }
      if (stage === "awaiting_proof") {
        if (message.type !== proofType) {
          finish(new ReceiverProtocolError("challenge_response_invalid"));
          closeSocket(socket);
          return;
        }
        stage = "verifying_proof";
        try {
          proofMessage = message;
          const secretRequest = await buildSecretRequest(message);
          if (settled) return;
          socket.send(JSON.stringify(secretRequest));
          stage = "awaiting_result";
        } catch (error) {
          finish(error);
          closeSocket(socket);
        }
        return;
      }
      if (message.type !== expectedType) {
        finish(new ReceiverProtocolError("protocol_mismatch"));
        return;
      }
      try {
        validateFinal(message, proofMessage);
        finish(null, message);
      } catch (error) {
        finish(error);
      }
    };
    const handleError = () => {
      finish(new DirectConnectionError(
        connectionError.code,
        connectionError.message
      ));
    };
    const handleClose = () => {
      finish(new DirectConnectionError(
        "receiver_closed",
        "Receiver 在挑战证明或鉴权完成前断开"
      ));
    };
    const handleAbort = () => {
      finish(new ReceiverRecoveryCancelledError());
      closeSocket(socket);
    };

    timeout = setTimeout(() => {
      finish(new ReceiverProtocolError(
        stage === "awaiting_result"
          ? "handshake_timeout"
          : "challenge_timeout"
      ));
      closeSocket(socket);
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

function controlMessageExceedsLimit(value) {
  if (value.length > DIRECT_MAX_CONTROL_TEXT_BYTES) {
    return true;
  }
  return controlTextEncoder.encode(value).byteLength >
    DIRECT_MAX_CONTROL_TEXT_BYTES;
}

function connectionErrorForHost(host) {
  return normalizeReceiverHost(host) === DEFAULT_RECEIVER_HOST
    ? {
        code: "automatic_address_or_connection_failed",
        message: "自动地址解析或 Receiver 连接失败，请改用平板显示的数字 IPv4"
      }
    : {
        code: "receiver_connection_failed",
        message: "无法连接 Receiver，请确认平板已开始接收且数字 IPv4 正确"
      };
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

class DirectConnectionError extends Error {
  constructor(code, message) {
    super(message);
    this.name = "DirectConnectionError";
    this.code = code;
    this.recoverable = true;
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
    short_code_requires_manual_ipv4:
      "短码连接需在“平板地址”填写平板显示的数字私网 IPv4",
    protocol_mismatch: "电脑扩展与平板应用版本不兼容，请同时更新后重试",
    codec_unsupported: "当前平板无法播放这组视频参数",
    epoch_stale: "平板已切换到更新的页面来源",
    invalid_response: "平板返回了无效的连接响应",
    unsolicited_response: "Receiver 在请求发送前返回了响应，已拒绝该连接",
    challenge_timeout: "Receiver 挑战证明超时，未发送任何凭据",
    challenge_invalid: "Receiver 拒绝了无效挑战，未发送任何凭据",
    challenge_state_invalid: "Receiver 拒绝了乱序或不匹配的挑战消息",
    challenge_required: "Receiver 未在时限内完成挑战证明",
    control_message_too_large: "Receiver 控制消息超过 2048 字节，已拒绝连接",
    challenge_response_invalid: "Receiver 返回了无效的挑战证明，未发送任何凭据",
    challenge_proof_invalid: "Receiver 挑战证明校验失败，未发送任何凭据",
    handshake_timeout: "WebSocket 已建立，但 Receiver 鉴权响应超时"
  }[code] ?? `平板拒绝连接（${String(code ?? "unknown").slice(0, 64)}）`;
}
