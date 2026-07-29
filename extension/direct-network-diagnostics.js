import {
  DEFAULT_RECEIVER_HOST,
  DIRECT_PORT,
  normalizeReceiverHost
} from "./direct-protocol.js";

const OBSERVATION_TTL_MS = 15_000;
const RESULT_WAIT_MS = 1_000;
const RESULT_POLL_MS = 10;
const OBSERVATION_PAGE_PATHS = Object.freeze([
  "setup.html",
  "offscreen.html"
]);

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

export function createDirectObservationContext(sender, runtimeApi) {
  const extensionId = String(runtimeApi?.id ?? "");
  if (typeof runtimeApi?.getURL !== "function") {
    throw new Error("自动地址检查无法确认扩展运行时来源");
  }
  if (extensionId.length === 0) {
    throw new Error("自动地址检查缺少扩展运行时标识");
  }

  const runtimeRoot = new URL(runtimeApi.getURL(""));
  const initiator = canonicalOrigin(runtimeRoot);
  const allowedUrls = new Map(
    OBSERVATION_PAGE_PATHS.map((path) => [
      new URL(runtimeApi.getURL(path)).href,
      path
    ])
  );
  const senderUrl = optionalUrl(sender?.url);
  if (senderUrl === null) {
    throw new Error("自动地址检查缺少扩展页面 URL");
  }
  const page = allowedUrls.get(senderUrl);
  if (page === undefined) {
    throw new Error("自动地址检查只接受扩展配对页或捕获页的文档上下文");
  }
  if (sender?.id !== undefined && sender.id !== extensionId) {
    throw new Error("自动地址检查的来源扩展不匹配");
  }
  if (
    sender?.documentId !== undefined &&
    optionalString(sender.documentId) === null
  ) {
    throw new Error("自动地址检查的文档标识格式无效");
  }
  if (
    sender?.origin !== undefined &&
    optionalOrigin(sender.origin) !== initiator
  ) {
    throw new Error("自动地址检查的来源 origin 不匹配");
  }
  return Object.freeze({
    documentId: optionalString(sender?.documentId),
    initiator,
    page
  });
}

export class DirectRequestObserver {
  constructor({ now = () => Date.now() } = {}) {
    this.now = now;
    this.attempts = new Map();
  }

  begin(url, context) {
    this.prune();
    const parsed = validateDirectUrl(url);
    const validatedContext = validateObservationContext(context);
    const attemptId = crypto.randomUUID();
    this.attempts.set(attemptId, {
      url: parsed.href,
      ...validatedContext,
      requestId: null,
      boundDocumentId: validatedContext.documentId,
      observedAddressClass: "unresolved",
      addressObserved: false,
      terminalObserved: false,
      contextInvalid: false,
      expiresAt: this.now() + OBSERVATION_TTL_MS
    });
    return attemptId;
  }

  observeBefore(details) {
    if (details?.requestId === undefined) return;
    const candidates = [...this.attempts.entries()]
      .filter(([, attempt]) =>
        attempt.requestId === null &&
        attempt.url === details?.url &&
        matchesInitialEventContext(attempt, details) &&
        attempt.expiresAt >= this.now())
    if (candidates.length !== 1) {
      for (const [, attempt] of candidates) {
        attempt.contextInvalid = true;
      }
      return;
    }
    const attempt = candidates[0][1];
    attempt.requestId = String(details.requestId);
    attempt.boundDocumentId =
      optionalString(details?.documentId) ?? attempt.boundDocumentId;
  }

  observeResponse(details) {
    this.observeAddress(details, false);
  }

  observeTerminal(details) {
    this.observeAddress(details, true);
  }

  observeAddress(details, terminal) {
    const attempt = [...this.attempts.values()].find(
      (candidate) => candidate.requestId === String(details?.requestId)
    );
    if (!attempt) return;
    if (!matchesBoundEventContext(attempt, details)) {
      attempt.contextInvalid = true;
    } else if (details?.ip) {
      const observedAddressClass = classifyObservedAddress(details.ip);
      if (
        attempt.addressObserved &&
        attempt.observedAddressClass !== observedAddressClass
      ) {
        attempt.observedAddressClass = "non_private";
      } else {
        attempt.observedAddressClass = observedAddressClass;
      }
      attempt.addressObserved = true;
    }
    if (terminal) attempt.terminalObserved = true;
  }

  finish(attemptId, socketOutcome, context) {
    const attempt = this.attempts.get(String(attemptId));
    if (!attempt || !matchesObservationContext(attempt, context)) {
      return {
        observedAddressClass: "unresolved",
        socketOutcome
      };
    }
    this.attempts.delete(String(attemptId));
    if (
      attempt.expiresAt < this.now() ||
      attempt.requestId === null ||
      !attempt.terminalObserved ||
      attempt.contextInvalid
    ) {
      return {
        observedAddressClass: "unresolved",
        socketOutcome
      };
    }
    return {
      observedAddressClass: attempt.observedAddressClass,
      socketOutcome
    };
  }

  async finishWhenReady(
    attemptId,
    socketOutcome,
    waitMs = RESULT_WAIT_MS,
    context
  ) {
    const attempt = this.attempts.get(String(attemptId));
    if (!attempt || !matchesObservationContext(attempt, context)) {
      return {
        observedAddressClass: "unresolved",
        socketOutcome
      };
    }
    const boundedWaitMs = Math.max(
      0,
      Math.min(Number(waitMs) || 0, RESULT_WAIT_MS)
    );
    const deadline = Date.now() + boundedWaitMs;
    while (
      this.attempts.get(String(attemptId))?.terminalObserved === false &&
      Date.now() < deadline
    ) {
      await new Promise((resolve) => setTimeout(
        resolve,
        Math.min(RESULT_POLL_MS, Math.max(0, deadline - Date.now()))
      ));
    }
    return this.finish(attemptId, socketOutcome, context);
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
    (details) => observer.observeResponse(details),
    filter
  );
  webRequestApi.onCompleted.addListener(
    (details) => observer.observeTerminal(details),
    filter
  );
  webRequestApi.onErrorOccurred.addListener(
    (details) => observer.observeTerminal(details),
    filter
  );
}

export async function beginDirectTransportObservation(url) {
  if (!globalThis.chrome?.runtime?.sendMessage) {
    throw new Error("自动地址安全检查不可用，已拒绝建立 Receiver 连接");
  }
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
      observedAddressClass: "unresolved"
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

export function handleDirectObservationMessage(
  message,
  sender,
  { observer, runtimeApi }
) {
  if (message?.type === "BEGIN_DIRECT_OBSERVATION") {
    try {
      return {
        handled: true,
        keepChannelOpen: false,
        response: {
          ok: true,
          attemptId: observer.begin(
            message.url,
            createDirectObservationContext(sender, runtimeApi)
          )
        }
      };
    } catch (error) {
      return {
        handled: true,
        keepChannelOpen: false,
        response: {
          ok: false,
          error: normalizeObservationError(error)
        }
      };
    }
  }
  if (message?.type === "FINISH_DIRECT_OBSERVATION") {
    try {
      const context = createDirectObservationContext(sender, runtimeApi);
      return {
        handled: true,
        keepChannelOpen: true,
        responsePromise: observer.finishWhenReady(
          message.attemptId,
          message.socketOutcome,
          message.waitMs,
          context
        ).then((observation) => ({ ok: true, observation }))
          .catch((error) => ({
            ok: false,
            error: normalizeObservationError(error)
          }))
      };
    } catch (error) {
      return {
        handled: true,
        keepChannelOpen: false,
        response: {
          ok: false,
          error: normalizeObservationError(error)
        }
      };
    }
  }
  return {
    handled: false,
    keepChannelOpen: false
  };
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

function validateObservationContext(value) {
  const documentId = optionalString(value?.documentId);
  const initiator = String(value?.initiator ?? "");
  const page = String(value?.page ?? "");
  if (
    initiator.length === 0 ||
    !OBSERVATION_PAGE_PATHS.includes(page)
  ) {
    throw new Error("自动地址检查缺少可信扩展文档上下文");
  }
  return {
    documentId,
    initiator,
    page
  };
}

function matchesInitialEventContext(attempt, details) {
  if (details?.url !== attempt.url) return false;
  if (
    hasInvalidOptionalString(details, "initiator") ||
    hasInvalidOptionalString(details, "documentId")
  ) {
    return false;
  }
  const eventInitiator = optionalOrigin(details?.initiator);
  const eventDocumentId = optionalString(details?.documentId);
  if (eventInitiator !== null && eventInitiator !== attempt.initiator) {
    return false;
  }
  if (
    eventDocumentId !== null &&
    attempt.documentId !== null &&
    eventDocumentId !== attempt.documentId
  ) {
    return false;
  }
  return eventInitiator === attempt.initiator ||
    (eventDocumentId !== null && eventDocumentId === attempt.documentId);
}

function matchesBoundEventContext(attempt, details) {
  if (details?.url !== attempt.url) return false;
  if (
    hasInvalidOptionalString(details, "initiator") ||
    hasInvalidOptionalString(details, "documentId")
  ) {
    return false;
  }
  const eventInitiator = optionalOrigin(details?.initiator);
  const eventDocumentId = optionalString(details?.documentId);
  if (eventInitiator !== null && eventInitiator !== attempt.initiator) {
    return false;
  }
  if (
    eventDocumentId !== null &&
    attempt.boundDocumentId !== null &&
    eventDocumentId !== attempt.boundDocumentId
  ) {
    return false;
  }
  if (attempt.boundDocumentId === null && eventDocumentId !== null) {
    attempt.boundDocumentId = eventDocumentId;
  }
  return true;
}

function matchesObservationContext(attempt, context) {
  let validatedContext;
  try {
    validatedContext = validateObservationContext(context);
  } catch {
    return false;
  }
  if (
    validatedContext.initiator !== attempt.initiator ||
    validatedContext.page !== attempt.page
  ) {
    return false;
  }
  const expectedDocumentId =
    attempt.boundDocumentId ?? attempt.documentId;
  return expectedDocumentId === null ||
    validatedContext.documentId === null ||
    validatedContext.documentId === expectedDocumentId;
}

function optionalString(value) {
  if (value === undefined || value === null || value === "") return null;
  return typeof value === "string" ? value : null;
}

function hasInvalidOptionalString(source, key) {
  return source?.[key] !== undefined &&
    source[key] !== null &&
    (typeof source[key] !== "string" || source[key].length === 0);
}

function optionalUrl(value) {
  if (value === undefined || value === null || value === "") return null;
  try {
    return new URL(String(value)).href;
  } catch {
    return null;
  }
}

function optionalOrigin(value) {
  if (value === undefined || value === null || value === "") return null;
  try {
    return canonicalOrigin(new URL(String(value)));
  } catch {
    return "";
  }
}

function canonicalOrigin(url) {
  const standardOrigin = url.origin;
  return standardOrigin === "null"
    ? `${url.protocol}//${url.host}`
    : standardOrigin;
}

function normalizeObservationError(error) {
  return error instanceof Error ? error.message : String(error);
}
