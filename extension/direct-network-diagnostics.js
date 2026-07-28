import {
  DEFAULT_RECEIVER_HOST,
  DIRECT_PORT,
  normalizeReceiverHost
} from "./direct-protocol.js";

const OBSERVATION_TTL_MS = 15_000;
const RESULT_WAIT_MS = 250;

export function classifyObservedAddress(value) {
  const octets = String(value ?? "").split(".");
  if (
    octets.length !== 4 ||
    octets.some((part) => !/^\d{1,3}$/.test(part)) ||
    octets.some((part) => Number(part) > 255)
  ) {
    return "non_private";
  }
  const [first, second] = octets.map(Number);
  return first === 10 ||
    (first === 172 && second >= 16 && second <= 31) ||
    (first === 192 && second === 168) ||
    (first === 169 && second === 254)
    ? "private_ipv4"
    : "non_private";
}

export function describeDirectTransportFailure({
  host,
  socketOutcome,
  observedAddressClass = "unresolved"
}) {
  const normalizedHost = normalizeReceiverHost(host);
  const automatic = normalizedHost === DEFAULT_RECEIVER_HOST;
  if (automatic && observedAddressClass === "non_private") {
    return {
      code: "automatic_address_not_private",
      message:
        "自动地址解析到了非私网地址，已拒绝连接；请改用平板显示的数字 IPv4",
      allowAuthentication: false,
      recoverable: false
    };
  }
  if (socketOutcome === "open") {
    if (automatic && observedAddressClass !== "private_ipv4") {
      return {
        code: "automatic_address_unverified",
        message:
          "无法验证自动地址的私网归属，已拒绝发送凭据；请改用平板显示的数字 IPv4",
        allowAuthentication: false,
        recoverable: false
      };
    }
    return {
      code: "connected",
      message: "",
      allowAuthentication: true,
      recoverable: true
    };
  }
  if (!automatic) {
    return {
      code: socketOutcome === "timeout"
        ? "manual_address_timeout"
        : "manual_address_unreachable",
      message: socketOutcome === "timeout"
        ? "数字 IPv4 连接超时；请核对平板显示的地址和局域网隔离设置"
        : "数字 IPv4 已接受，但 Receiver WebSocket 不可达；请核对平板显示的地址",
      allowAuthentication: false,
      recoverable: true
    };
  }
  if (observedAddressClass !== "private_ipv4") {
    return {
      code: socketOutcome === "timeout"
        ? "automatic_address_resolution_timeout"
        : "automatic_address_resolution_failed",
      message: socketOutcome === "timeout"
        ? "自动地址解析超时；请改用平板显示的数字 IPv4"
        : "自动地址解析失败；请改用平板显示的数字 IPv4",
      allowAuthentication: false,
      recoverable: true
    };
  }
  return {
    code: socketOutcome === "timeout"
      ? "automatic_address_connection_timeout"
      : "automatic_address_unreachable",
    message: socketOutcome === "timeout"
      ? "已解析到平板私网地址，但连接超时；请确认局域网未隔离"
      : "已解析到平板私网地址，但 Receiver WebSocket 不可达；请确认 Receiver 正在接收",
    allowAuthentication: false,
    recoverable: true
  };
}

export class DirectRequestObserver {
  constructor({ now = () => Date.now() } = {}) {
    this.now = now;
    this.attempts = new Map();
  }

  begin(url) {
    this.prune();
    const parsed = validateDirectUrl(url);
    const attemptId = crypto.randomUUID();
    this.attempts.set(attemptId, {
      url: parsed.href,
      requestId: null,
      observedAddressClass: "unresolved",
      terminalObserved: false,
      expiresAt: this.now() + OBSERVATION_TTL_MS
    });
    return attemptId;
  }

  observeBefore(details) {
    const candidate = [...this.attempts.entries()]
      .filter(([, attempt]) =>
        attempt.requestId === null &&
        attempt.url === details?.url &&
        attempt.expiresAt >= this.now())
      .at(-1);
    if (!candidate) return;
    candidate[1].requestId = String(details.requestId);
  }

  observeAddress(details) {
    const attempt = [...this.attempts.values()].find(
      (candidate) => candidate.requestId === String(details?.requestId)
    );
    if (!attempt) return;
    attempt.observedAddressClass = details?.ip
      ? classifyObservedAddress(details.ip)
      : "unresolved";
    attempt.terminalObserved = true;
  }

  finish(attemptId, socketOutcome) {
    const attempt = this.attempts.get(String(attemptId));
    if (!attempt || attempt.expiresAt < this.now()) {
      return {
        observedAddressClass: "unresolved",
        socketOutcome
      };
    }
    this.attempts.delete(String(attemptId));
    return {
      observedAddressClass: attempt.observedAddressClass,
      socketOutcome
    };
  }

  async finishWhenReady(attemptId, socketOutcome, waitMs = RESULT_WAIT_MS) {
    const deadline = this.now() + Math.max(0, Math.min(Number(waitMs) || 0, 500));
    while (
      this.attempts.get(String(attemptId))?.terminalObserved === false &&
      this.now() < deadline
    ) {
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    return this.finish(attemptId, socketOutcome);
  }

  prune() {
    const now = this.now();
    for (const [attemptId, attempt] of this.attempts) {
      if (attempt.expiresAt < now) this.attempts.delete(attemptId);
    }
  }
}

export function installDirectRequestObserver(observer, webRequestApi) {
  const filter = {
    urls: ["ws://*/*"],
    types: ["websocket"]
  };
  webRequestApi.onBeforeRequest.addListener(
    (details) => observer.observeBefore(details),
    filter
  );
  webRequestApi.onResponseStarted.addListener(
    (details) => observer.observeAddress(details),
    filter
  );
  webRequestApi.onErrorOccurred.addListener(
    (details) => observer.observeAddress(details),
    filter
  );
}

export async function beginDirectTransportObservation(url) {
  if (!globalThis.chrome?.runtime?.sendMessage) return null;
  const response = await chrome.runtime.sendMessage({
    target: "service-worker",
    type: "BEGIN_DIRECT_OBSERVATION",
    url
  });
  if (!response?.ok || !response.attemptId) {
    throw new Error(response?.error || "无法启动自动地址安全检查");
  }
  return response.attemptId;
}

export async function finishDirectTransportObservation(
  attemptId,
  host,
  socketOutcome
) {
  if (attemptId === null) {
    return describeDirectTransportFailure({
      host,
      socketOutcome,
      observedAddressClass: normalizeReceiverHost(host) === DEFAULT_RECEIVER_HOST
        ? "private_ipv4"
        : "unresolved"
    });
  }
  const response = await chrome.runtime.sendMessage({
    target: "service-worker",
    type: "FINISH_DIRECT_OBSERVATION",
    attemptId,
    socketOutcome,
    waitMs: RESULT_WAIT_MS
  });
  if (!response?.ok || !response.observation) {
    throw new Error(response?.error || "无法读取自动地址安全检查结果");
  }
  return describeDirectTransportFailure({
    host,
    ...response.observation
  });
}

export class DirectTransportError extends Error {
  constructor(verdict) {
    super(verdict.message);
    this.name = "DirectTransportError";
    this.code = verdict.code;
    this.recoverable = verdict.recoverable;
  }
}

function validateDirectUrl(value) {
  const url = new URL(String(value ?? ""));
  const host = normalizeReceiverHost(url.hostname);
  if (
    url.protocol !== "ws:" ||
    Number(url.port) !== DIRECT_PORT ||
    url.pathname !== "/direct" ||
    url.search !== "" ||
    url.hash !== "" ||
    url.hostname !== host
  ) {
    throw new Error("自动地址检查只允许 Receiver 直连入口");
  }
  return url;
}
