import {
  createDirectWebSocketUrl,
  DIRECT_MAX_BUFFERED_BYTES,
  DIRECT_PROTOCOL,
  DIRECT_VIDEO_FRAMERATE,
  DIRECT_VIDEO_HEIGHT,
  DIRECT_VIDEO_WIDTH,
  validateTrustedDevice
} from "./direct-protocol.js";

const CONNECT_TIMEOUT_MS = 5_000;

export async function pairReceiver({
  host,
  authorization,
  senderId,
  socketFactory = (url) => new WebSocket(url)
}) {
  const socket = socketFactory(createDirectWebSocketUrl(host));
  const response = await openAndExchange(socket, {
    type: "pair",
    protocol: DIRECT_PROTOCOL,
    sessionId: authorization.sessionId,
    token: authorization.token,
    senderId
  }, "paired");
  const trusted = validateTrustedDevice({
    senderId,
    deviceId: response.deviceId,
    credential: response.credential,
    host,
    pairedAt: Date.now()
  });
  socket.close(1000, "pairing_complete");
  return trusted;
}

export class DirectReceiverConnection {
  constructor({
    trustedDevice,
    sourceEpoch,
    socketFactory = (url) => new WebSocket(url),
    heartbeatIntervalMs = 5_000
  }) {
    this.trustedDevice = validateTrustedDevice(trustedDevice);
    this.sourceEpoch = sourceEpoch;
    this.socketFactory = socketFactory;
    this.heartbeatIntervalMs = heartbeatIntervalMs;
    this.socket = null;
    this.onControl = () => {};
    this.onClose = () => {};
    this.heartbeatTimer = null;
  }

  async connect() {
    const socket = this.socketFactory(
      createDirectWebSocketUrl(this.trustedDevice.host)
    );
    const response = await openAndExchange(socket, {
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
    }, "ready");
    if (response.sourceEpoch !== this.sourceEpoch) {
      socket.close(1008, "epoch_mismatch");
      throw new Error("平板返回了过期的页面来源状态");
    }
    this.socket = socket;
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
    return response;
  }

  get connected() {
    return this.socket?.readyState === WebSocket.OPEN;
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

function openAndExchange(socket, request, expectedType) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const timeout = setTimeout(() => {
      finish(new Error("连接平板超时"));
      socket.close();
    }, CONNECT_TIMEOUT_MS);

    const finish = (error, value) => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timeout);
      if (error) {
        reject(error);
      } else {
        resolve(value);
      }
    };

    socket.binaryType = "arraybuffer";
    socket.addEventListener("open", () => {
      socket.send(JSON.stringify(request));
    }, { once: true });
    socket.addEventListener("message", (event) => {
      if (typeof event.data !== "string") {
        finish(new Error("平板在连接过程中返回了无法识别的数据"));
        return;
      }
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        finish(new Error("平板返回了无法识别的响应"));
        return;
      }
      if (message?.type === "error") {
        finish(new Error(describeReceiverError(message.code)));
        return;
      }
      if (
        message?.type !== expectedType ||
        message.protocol !== DIRECT_PROTOCOL
      ) {
        finish(new Error("平板返回了无效的连接响应"));
        return;
      }
      finish(null, message);
    });
    socket.addEventListener("error", () => {
      finish(new Error("无法连接平板，请检查电脑和平板是否在同一 Wi-Fi，以及平板地址是否正确"));
    });
    socket.addEventListener("close", () => {
      finish(new Error("平板在连接完成前断开"));
    });
  });
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
    epoch_stale: "平板已切换到更新的页面来源"
  }[code] ?? `平板拒绝连接（${String(code ?? "unknown").slice(0, 64)}）`;
}
