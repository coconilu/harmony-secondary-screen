import { validateReceiverConfig } from "./receiver-config.js";

const STATE_KEY = "captureProbeState";
const SETUP_PAGE = "setup.html";
const MONITOR_PAGE = "monitor.html";
const CAPTURABLE_SCHEMES = new Set(["http:", "https:"]);
const NATIVE_RELAY_HOST = "com.coconilu.harmony_web_companion";
const NATIVE_RELAY_PROTOCOL = 1;

let updateQueue = Promise.resolve();
let startInFlight = false;
let nativeRelayPort = null;
const expectedNativeRelayDisconnects = new WeakSet();

chrome.runtime.onInstalled.addListener(() => {
  void resetProbe();
});

chrome.runtime.onStartup.addListener(() => {
  void resetProbe();
});

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.target !== "service-worker") {
    return false;
  }

  if (message.type === "START_CAPTURE") {
    if (startInFlight) {
      sendResponse({ ok: false, error: "正在启动，请勿重复操作" });
      return false;
    }
    startInFlight = true;
    startProbeFromRequest(message.receiver)
      .then((state) => sendResponse({ ok: true, state }))
      .catch((error) => sendResponse({ ok: false, error: normalizeError(error) }))
      .finally(() => {
        startInFlight = false;
      });
    return true;
  }

  if (message.type === "STOP_CAPTURE") {
    stopProbe("user_stopped_capture")
      .then((state) => sendResponse({ ok: true, state }))
      .catch((error) => sendResponse({ ok: false, error: normalizeError(error) }));
    return true;
  }

  if (message.type === "RESET_PROBE") {
    resetProbe()
      .then((state) => sendResponse({ ok: true, state }))
      .catch((error) => sendResponse({ ok: false, error: normalizeError(error) }));
    return true;
  }

  if (message.type === "SET_STAGE") {
    setStage(message.stage)
      .then((state) => sendResponse({ ok: true, state }))
      .catch((error) => sendResponse({ ok: false, error: normalizeError(error) }));
    return true;
  }

  if (message.type === "CAPTURE_TELEMETRY") {
    void applyTelemetry(message.telemetry);
    sendResponse({ ok: true });
    return false;
  }

  if (message.type === "CAPTURE_ENDED") {
    void finishUnexpectedCapture(message.reason, message.telemetry);
    sendResponse({ ok: true });
    return false;
  }

  if (message.type === "CAPTURE_FAILURE") {
    void failProbe(message.error, message.telemetry);
    sendResponse({ ok: true });
    return false;
  }

  return false;
});

async function startProbeFromRequest(receiver) {
  const [tab] = await chrome.tabs.query({
    active: true,
    currentWindow: true
  });
  return startProbeFromAction(tab, receiver);
}

async function startProbeFromAction(tab, receiver) {
  try {
    validateCapturableTab(tab);
    const receiverConfig = validateReceiverConfig(receiver);
    const streamIdPromise = chrome.tabCapture.getMediaStreamId({
      targetTabId: tab.id
    });

    await setState({
      ...createInitialState(),
      mode: "starting",
      startedAt: Date.now(),
      updatedAt: Date.now(),
      events: [createEvent("INFO", "正在申请当前标签页视频轨")]
    });
    await updateAction("starting");

    const streamId = await streamIdPromise;
    await ensureOffscreenDocument();
    const relayInfo = await startLocalRelay(receiverConfig);

    const response = await chrome.runtime.sendMessage({
      target: "offscreen",
      type: "START_CAPTURE",
      streamId,
      relayInfo
    });
    if (!response?.ok) {
      throw new Error(response?.error || "offscreen 捕获启动失败");
    }

    await applyTelemetry(response.telemetry, {
      mode: "capturing",
      event: [
        "INFO",
        "当前标签页已通过 Relay 发送到 HarmonyOS 平板（不采集音频）"
      ],
      beginStage: true
    });
    await chrome.action.setPopup({ popup: MONITOR_PAGE });
    await updateAction("capturing");
    return getState();
  } catch (error) {
    await failProbe(normalizeError(error));
    throw error;
  }
}

async function stopProbe(reason) {
  await updateState((state) => ({
    ...state,
    mode: "stopping",
    updatedAt: Date.now(),
    events: appendEvent(state.events, "INFO", "正在停止捕获")
  }));
  await updateAction("stopping");

  let telemetry = null;
  if (await hasOffscreenDocument()) {
    const response = await chrome.runtime.sendMessage({
      target: "offscreen",
      type: "STOP_CAPTURE"
    });
    if (!response?.ok) {
      throw new Error(response?.error || "停止 offscreen 捕获失败");
    }
    telemetry = response.telemetry;
    await chrome.offscreen.closeDocument();
  }
  stopLocalRelay();

  const stoppedAt = Date.now();
  const state = await updateState((current) => {
    const next = {
      ...current,
      ...telemetryToState(telemetry),
      mode: "stopped",
      stoppedAt,
      updatedAt: stoppedAt,
      stopReason: reason,
      events: appendEvent(current.events, "INFO", "捕获与编码已停止")
    };
    return {
      ...next,
      stageRuns: closeActiveStage(next.stageRuns, next, stoppedAt)
    };
  });
  await updateAction("stopped");
  return state;
}

async function resetProbe() {
  if (await hasOffscreenDocument()) {
    try {
      await chrome.runtime.sendMessage({
        target: "offscreen",
        type: "STOP_CAPTURE"
      });
    } catch {
      // The stale document can be closed directly.
    }
    await chrome.offscreen.closeDocument();
  }
  stopLocalRelay();

  const state = createInitialState();
  await setState(state);
  await chrome.action.setPopup({ popup: SETUP_PAGE });
  await updateAction("idle");
  return state;
}

async function setStage(stage) {
  const validStages = new Set(["A", "B", "C", "D"]);
  if (!validStages.has(stage)) {
    throw new Error("未知测试阶段");
  }

  return updateState((state) => {
    if (state.mode !== "capturing") {
      throw new Error("只能在捕获进行时切换测试阶段");
    }
    if (state.currentStage === stage) {
      return state;
    }

    const changedAt = Date.now();
    return {
      ...state,
      currentStage: stage,
      updatedAt: changedAt,
      stageRuns: beginStage(
        closeActiveStage(state.stageRuns, state, changedAt),
        stage,
        state,
        changedAt
      ),
      events: appendEvent(state.events, "STEP", `切换到测试阶段 ${stage}`)
    };
  });
}

async function applyTelemetry(telemetry, options = {}) {
  if (!telemetry) {
    return;
  }

  const state = await updateState((current) => {
    let events = current.events;
    if (options.event) {
      events = appendEvent(events, options.event[0], options.event[1]);
    }
    if (telemetry.transition === "stalled") {
      events = appendEvent(events, "WARN", "连续 2 秒没有收到新视频帧");
    } else if (telemetry.transition === "recovered") {
      events = appendEvent(events, "INFO", "视频帧已恢复");
    }

    const next = {
      ...current,
      ...telemetryToState(telemetry),
      mode: options.mode ?? current.mode,
      updatedAt: Date.now(),
      events
    };
    if (options.beginStage && next.stageRuns.length === 0) {
      next.stageRuns = beginStage(
        next.stageRuns,
        next.currentStage,
        next,
        next.updatedAt
      );
    }
    return next;
  });
  await updateAction(state.stalled ? "stalled" : state.mode);
}

async function finishUnexpectedCapture(reason, telemetry) {
  const stoppedAt = Date.now();
  const state = await updateState((current) => {
    const next = {
      ...current,
      ...telemetryToState(telemetry),
      mode: "stopped",
      stoppedAt,
      updatedAt: stoppedAt,
      stopReason: reason,
      events: appendEvent(current.events, "WARN", "视频轨意外结束")
    };
    return {
      ...next,
      stageRuns: closeActiveStage(next.stageRuns, next, stoppedAt)
    };
  });
  if (await hasOffscreenDocument()) {
    await chrome.offscreen.closeDocument();
  }
  stopLocalRelay();
  await updateAction(state.mode);
}

async function failProbe(errorMessage, telemetry = null) {
  const stoppedAt = Date.now();
  const state = await updateState((current) => {
    const next = {
      ...current,
      ...telemetryToState(telemetry),
      mode: "error",
      error: errorMessage,
      stoppedAt,
      updatedAt: stoppedAt,
      events: appendEvent(current.events, "ERROR", errorMessage)
    };
    return {
      ...next,
      stageRuns: closeActiveStage(next.stageRuns, next, stoppedAt)
    };
  });

  if (await hasOffscreenDocument()) {
    try {
      await chrome.offscreen.closeDocument();
    } catch {
      // Closing is best effort after a capture failure.
    }
  }
  stopLocalRelay();
  await chrome.action.setPopup({ popup: MONITOR_PAGE });
  await updateAction(state.mode);
}

async function ensureOffscreenDocument() {
  if (await hasOffscreenDocument()) {
    return;
  }

  await chrome.offscreen.createDocument({
    url: "offscreen.html",
    reasons: ["USER_MEDIA"],
    justification: "持有用户主动选择的标签页视频流并统计帧是否持续到达"
  });
}

async function hasOffscreenDocument() {
  const offscreenUrl = chrome.runtime.getURL("offscreen.html");
  const contexts = await chrome.runtime.getContexts({
    contextTypes: ["OFFSCREEN_DOCUMENT"],
    documentUrls: [offscreenUrl]
  });
  return contexts.length > 0;
}

function validateCapturableTab(tab) {
  if (!tab?.id || !tab.url) {
    throw new Error("无法取得当前活动标签页");
  }

  const url = new URL(tab.url);
  if (!CAPTURABLE_SCHEMES.has(url.protocol)) {
    throw new Error("当前页面不能捕获，请在普通 HTTP/HTTPS 网页上开始测试");
  }
}

async function updateAction(mode) {
  const variants = {
    idle: { text: "", color: "#157a78", title: "开始标签页捕获验证" },
    starting: { text: "…", color: "#157a78", title: "正在启动捕获" },
    capturing: { text: "REC", color: "#157a78", title: "查看捕获状态" },
    stalled: { text: "!", color: "#b4232f", title: "检测到视频帧定格" },
    stopping: { text: "…", color: "#7a8797", title: "正在停止捕获" },
    stopped: { text: "DONE", color: "#52657a", title: "查看捕获结果" },
    error: { text: "ERR", color: "#b4232f", title: "捕获启动失败" }
  };
  const variant = variants[mode] ?? variants.idle;
  await Promise.all([
    chrome.action.setBadgeText({ text: variant.text }),
    chrome.action.setBadgeBackgroundColor({ color: variant.color }),
    chrome.action.setTitle({ title: variant.title })
  ]);
}

function createInitialState() {
  return {
    mode: "idle",
    currentStage: "A",
    fps: 0,
    totalFrames: 0,
    lastFrameAgeMs: null,
    stallEvents: 0,
    stalled: false,
    encodingFps: 0,
    submittedFrames: 0,
    encodedFrames: 0,
    encodedBytes: 0,
    keyFrames: 0,
    droppedFrames: 0,
    encodeErrors: 0,
    encodeQueueSize: 0,
    bitrateKbps: 0,
    averageBitrateKbps: 0,
    lastEncodedAgeMs: null,
    encoderConfig: null,
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
    receiverDroppedFrames: 0,
    startedAt: null,
    stoppedAt: null,
    updatedAt: Date.now(),
    stopReason: null,
    error: null,
    events: [],
    stageRuns: []
  };
}

function telemetryToState(telemetry) {
  if (!telemetry) {
    return {};
  }
  return {
    fps: telemetry.fps,
    totalFrames: telemetry.totalFrames,
    lastFrameAgeMs: telemetry.lastFrameAgeMs,
    stallEvents: telemetry.stallEvents,
    stalled: telemetry.stalled,
    encodingFps: telemetry.encodingFps,
    submittedFrames: telemetry.submittedFrames,
    encodedFrames: telemetry.encodedFrames,
    encodedBytes: telemetry.encodedBytes,
    keyFrames: telemetry.keyFrames,
    droppedFrames: telemetry.droppedFrames,
    encodeErrors: telemetry.encodeErrors,
    encodeQueueSize: telemetry.encodeQueueSize,
    bitrateKbps: telemetry.bitrateKbps,
    averageBitrateKbps: telemetry.averageBitrateKbps,
    lastEncodedAgeMs: telemetry.lastEncodedAgeMs,
    encoderConfig: telemetry.encoderConfig,
    relayConnected: telemetry.relayConnected,
    relaySentFrames: telemetry.relaySentFrames,
    relaySentBytes: telemetry.relaySentBytes,
    relayReceivedFrames: telemetry.relayReceivedFrames,
    relayReceivedBytes: telemetry.relayReceivedBytes,
    relayKeyFrames: telemetry.relayKeyFrames,
    relayInvalidMessages: telemetry.relayInvalidMessages,
    relayDroppedFrames: telemetry.relayDroppedFrames,
    relayBufferedAmount: telemetry.relayBufferedAmount,
    relayErrors: telemetry.relayErrors,
    relayLastErrorCode: telemetry.relayLastErrorCode,
    relayLastErrorDetail: telemetry.relayLastErrorDetail,
    lanConnected: telemetry.lanConnected,
    lanSentFrames: telemetry.lanSentFrames,
    lanSentBytes: telemetry.lanSentBytes,
    lanSentDatagrams: telemetry.lanSentDatagrams,
    lanSendErrors: telemetry.lanSendErrors,
    receiverDecodedFrames: telemetry.receiverDecodedFrames,
    receiverDroppedFrames: telemetry.receiverDroppedFrames
  };
}

function beginStage(stageRuns = [], stage, state, startedAt) {
  return [
    ...stageRuns,
    {
      stage,
      startedAt,
      endedAt: null,
      startTelemetry: createTelemetrySnapshot(state),
      endTelemetry: null
    }
  ];
}

function closeActiveStage(stageRuns = [], state, endedAt) {
  if (stageRuns.length === 0) {
    return stageRuns;
  }
  const lastIndex = stageRuns.length - 1;
  const active = stageRuns[lastIndex];
  if (active.endedAt !== null) {
    return stageRuns;
  }
  return [
    ...stageRuns.slice(0, lastIndex),
    {
      ...active,
      endedAt,
      endTelemetry: createTelemetrySnapshot(state)
    }
  ];
}

function createTelemetrySnapshot(state) {
  return {
    capturedFrames: state.totalFrames,
    encodedFrames: state.encodedFrames,
    encodedBytes: state.encodedBytes,
    droppedFrames: state.droppedFrames,
    stallEvents: state.stallEvents,
    encodeErrors: state.encodeErrors,
    relaySentFrames: state.relaySentFrames,
    relayReceivedFrames: state.relayReceivedFrames,
    relayDroppedFrames: state.relayDroppedFrames,
    relayErrors: state.relayErrors
  };
}

function startLocalRelay(receiver) {
  stopLocalRelay();

  return new Promise((resolve, reject) => {
    let settled = false;
    let timeout = null;
    let relayInfo = null;
    let port;
    try {
      port = chrome.runtime.connectNative(NATIVE_RELAY_HOST);
    } catch (error) {
      reject(
        new Error(
          `无法启动本地 Relay，请先完成 Native Host 注册：${normalizeError(error)}`
        )
      );
      return;
    }
    nativeRelayPort = port;

    const finishWithError = (message) => {
      if (settled) {
        return;
      }
      settled = true;
      if (timeout !== null) {
        clearTimeout(timeout);
      }
      reject(new Error(message));
    };

    port.onMessage.addListener((message) => {
      if (settled) {
        return;
      }
      if (message?.type === "error") {
        const code =
          typeof message.code === "string" ? message.code : "unknown_error";
        finishWithError(`连接平板失败：${describeReceiverError(code)}`);
        return;
      }
      if (relayInfo === null) {
        if (
          message?.type !== "ready" ||
          message.protocol !== NATIVE_RELAY_PROTOCOL ||
          !Number.isInteger(message.port) ||
          message.port < 1 ||
          message.port > 65535 ||
          typeof message.token !== "string" ||
          !/^[0-9a-f]{64}$/.test(message.token)
        ) {
          finishWithError("本地 Relay 返回了无效的启动信息");
          return;
        }
        relayInfo = {
          protocol: message.protocol,
          port: message.port,
          token: message.token
        };
        port.postMessage({
          type: "configure_receiver",
          receiverAddress: receiver.address,
          pairingCode: receiver.pairingCode
        });
        return;
      }
      if (message?.type !== "receiver_ready" || message.protocol !== 2) {
        finishWithError("Relay 返回了无效的平板配对结果");
        return;
      }
      settled = true;
      if (timeout !== null) clearTimeout(timeout);
      resolve(relayInfo);
    });

    port.onDisconnect.addListener(() => {
      const runtimeError = chrome.runtime.lastError?.message;
      if (expectedNativeRelayDisconnects.has(port)) {
        expectedNativeRelayDisconnects.delete(port);
        return;
      }
      if (!settled) {
        finishWithError(
          `本地 Relay 启动失败：${runtimeError || "Native Host 已断开"}`
        );
        return;
      }
      nativeRelayPort = null;
      void failProbe(
        `本地 Relay 意外退出：${runtimeError || "Native Host 已断开"}`
      );
    });

    timeout = setTimeout(() => {
      finishWithError("等待 Relay 与平板配对超时");
      stopLocalRelay();
    }, 10_000);
  });
}

function describeReceiverError(code) {
  return {
    receiver_address_not_allowed: "平板地址不属于允许的可信局域网",
    pairing_code_invalid: "配对码格式无效",
    receiver_unreachable: "无法连接平板，请检查同一 Wi-Fi 和地址",
    receiver_connect_timeout: "连接平板超时",
    pairing_failed: "配对码已过期或不匹配，请在平板重新开始接收",
    codec_unsupported: "平板拒绝 1280×720 H.264 Annex-B 参数",
    invalid_session: "平板返回了无效会话"
  }[code] ?? code;
}

function stopLocalRelay() {
  const port = nativeRelayPort;
  nativeRelayPort = null;
  if (!port) {
    return;
  }
  expectedNativeRelayDisconnects.add(port);
  try {
    port.postMessage({ type: "shutdown" });
  } catch {
    // The process may already have exited.
  }
  try {
    port.disconnect();
  } catch {
    // Disconnect is best effort during cleanup.
  }
}

function createEvent(level, message) {
  return {
    at: Date.now(),
    level,
    message
  };
}

function appendEvent(events = [], level, message) {
  return [...events, createEvent(level, message)].slice(-100);
}

async function getState() {
  const result = await chrome.storage.session.get(STATE_KEY);
  return result[STATE_KEY] ?? createInitialState();
}

async function setState(state) {
  await chrome.storage.session.set({ [STATE_KEY]: state });
  return state;
}

function updateState(mutator) {
  const operation = updateQueue.then(async () => {
    const current = await getState();
    const next = mutator(current);
    await setState(next);
    return next;
  });
  updateQueue = operation.then(
    () => undefined,
    () => undefined
  );
  return operation;
}

function normalizeError(error) {
  return error instanceof Error ? error.message : String(error);
}
