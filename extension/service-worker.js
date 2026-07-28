import {
  allocateSourceEpoch,
  getTrustedReceiver
} from "./pairing-store.js";

const STATE_KEY = "captureProbeState";
const SETUP_PAGE = "setup.html";
const MONITOR_PAGE = "monitor.html";
const CAPTURABLE_SCHEMES = new Set(["http:", "https:"]);

let updateQueue = Promise.resolve();
let startInFlight = false;

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
    startProbeFromRequest()
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

async function startProbeFromRequest() {
  const [tab] = await chrome.tabs.query({
    active: true,
    currentWindow: true
  });
  return startProbeFromAction(tab);
}

async function startProbeFromAction(tab) {
  try {
    validateCapturableTab(tab);
    const trustedDevice = await getTrustedReceiver();
    if (!trustedDevice) {
      throw new Error("请先用平板扫描二维码完成一次配对");
    }
    const sourceEpoch = await allocateSourceEpoch();
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

    const response = await chrome.runtime.sendMessage({
      target: "offscreen",
      type: "START_CAPTURE",
      streamId,
      directInfo: {
        trustedDevice,
        sourceEpoch
      }
    });
    if (!response?.ok) {
      throw new Error(response?.error || "offscreen 捕获启动失败");
    }

    await applyTelemetry(response.telemetry, {
      mode: "capturing",
      event: [
        "INFO",
        "当前标签页已直连发送到 HarmonyOS 平板（不采集音频）"
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
    if (!current.directReconnecting && telemetry.directReconnecting) {
      events = appendEvent(events, "WARN", "平板连接中断，正在保持捕获并自动恢复");
    } else if (
      current.directReconnecting &&
      !telemetry.directReconnecting &&
      telemetry.directConnected
    ) {
      events = appendEvent(events, "INFO", "平板连接已恢复，正在请求关键帧");
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
    directConnected: false,
    directSentFrames: 0,
    directSentBytes: 0,
    directDroppedFrames: 0,
    directBufferedAmount: 0,
    directErrors: 0,
    directKeyframeRequests: 0,
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
    directConnected: telemetry.directConnected,
    directSentFrames: telemetry.directSentFrames,
    directSentBytes: telemetry.directSentBytes,
    directDroppedFrames: telemetry.directDroppedFrames,
    directBufferedAmount: telemetry.directBufferedAmount,
    directErrors: telemetry.directErrors,
    directKeyframeRequests: telemetry.directKeyframeRequests,
    directReconnecting: telemetry.directReconnecting,
    directReconnectAttempts: telemetry.directReconnectAttempts,
    directReconnects: telemetry.directReconnects,
    directRecoveryState: telemetry.directRecoveryState,
    directRecoveryStartedAt: telemetry.directRecoveryStartedAt,
    directRecoveryElapsedMs: telemetry.directRecoveryElapsedMs,
    directRecoveryLastDurationMs: telemetry.directRecoveryLastDurationMs,
    directRecoveryTransientFailures: telemetry.directRecoveryTransientFailures,
    directRecoveryCurrentAttempt: telemetry.directRecoveryCurrentAttempt,
    directRecoveryLastOutcome: telemetry.directRecoveryLastOutcome,
    directLastCloseCode: telemetry.directLastCloseCode,
    directLastCloseWasClean: telemetry.directLastCloseWasClean,
    lastPongAt: telemetry.lastPongAt,
    receiverReceivedFrames: telemetry.receiverReceivedFrames,
    receiverReceivedBytes: telemetry.receiverReceivedBytes,
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
    directSentFrames: state.directSentFrames,
    receiverReceivedFrames: state.receiverReceivedFrames,
    directDroppedFrames: state.directDroppedFrames,
    directErrors: state.directErrors,
    directReconnectAttempts: state.directReconnectAttempts,
    directReconnects: state.directReconnects
  };
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
