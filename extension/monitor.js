const STATE_KEY = "captureProbeState";

const elements = {
  status: document.querySelector("#capture-status"),
  error: document.querySelector("#error-message"),
  fps: document.querySelector("#fps-value"),
  frames: document.querySelector("#frames-value"),
  encodingFps: document.querySelector("#encoding-fps-value"),
  encodedFrames: document.querySelector("#encoded-frames-value"),
  bitrate: document.querySelector("#bitrate-value"),
  age: document.querySelector("#age-value"),
  stalls: document.querySelector("#stalls-value"),
  droppedFrames: document.querySelector("#dropped-frames-value"),
  queueSize: document.querySelector("#queue-size-value"),
  encodeErrors: document.querySelector("#encode-errors-value"),
  encoderConfig: document.querySelector("#encoder-config"),
  directStatus: document.querySelector("#direct-status-value"),
  directSent: document.querySelector("#direct-sent-value"),
  directReceived: document.querySelector("#direct-received-value"),
  directDropped: document.querySelector("#direct-dropped-value"),
  directErrors: document.querySelector("#direct-errors-value"),
  log: document.querySelector("#event-log"),
  stop: document.querySelector("#stop-button"),
  reset: document.querySelector("#reset-button"),
  export: document.querySelector("#export-button"),
  stages: [...document.querySelectorAll("[data-stage]")]
};

elements.stop.addEventListener("click", stopCapture);
elements.reset.addEventListener("click", resetProbe);
elements.export.addEventListener("click", exportResult);
for (const button of elements.stages) {
  button.addEventListener("click", () => setStage(button.dataset.stage));
}

chrome.storage.onChanged.addListener((changes, areaName) => {
  if (areaName === "session" && changes[STATE_KEY]?.newValue) {
    render(changes[STATE_KEY].newValue);
  }
});

void loadState();

async function loadState() {
  const result = await chrome.storage.session.get(STATE_KEY);
  render(result[STATE_KEY] ?? createFallbackState());
}

async function stopCapture() {
  elements.stop.disabled = true;
  const response = await chrome.runtime.sendMessage({
    target: "service-worker",
    type: "STOP_CAPTURE"
  });
  if (!response?.ok) {
    showLocalError(response?.error || "停止捕获失败");
  }
}

async function resetProbe() {
  elements.reset.disabled = true;
  const response = await chrome.runtime.sendMessage({
    target: "service-worker",
    type: "RESET_PROBE"
  });
  if (!response?.ok) {
    showLocalError(response?.error || "重置测试失败");
    elements.reset.disabled = false;
    return;
  }
  window.close();
}

async function setStage(stage) {
  const response = await chrome.runtime.sendMessage({
    target: "service-worker",
    type: "SET_STAGE",
    stage
  });
  if (!response?.ok) {
    showLocalError(response?.error || "切换测试阶段失败");
  }
}

async function exportResult() {
  const result = await chrome.storage.session.get(STATE_KEY);
  const state = result[STATE_KEY] ?? createFallbackState();
  const safeResult = {
    schemaVersion: 5,
    exportedAt: new Date().toISOString(),
    mode: state.mode,
    currentStage: state.currentStage,
    fps: state.fps,
    totalFrames: state.totalFrames,
    lastFrameAgeMs: state.lastFrameAgeMs,
    stallEvents: state.stallEvents,
    stalled: state.stalled,
    encodingFps: state.encodingFps,
    submittedFrames: state.submittedFrames,
    encodedFrames: state.encodedFrames,
    encodedBytes: state.encodedBytes,
    keyFrames: state.keyFrames,
    droppedFrames: state.droppedFrames,
    encodeErrors: state.encodeErrors,
    encodeQueueSize: state.encodeQueueSize,
    bitrateKbps: state.bitrateKbps,
    averageBitrateKbps: state.averageBitrateKbps,
    lastEncodedAgeMs: state.lastEncodedAgeMs,
    encoderConfig: state.encoderConfig,
    directConnected: state.directConnected,
    directSentFrames: state.directSentFrames,
    directSentBytes: state.directSentBytes,
    directDroppedFrames: state.directDroppedFrames,
    directBufferedAmount: state.directBufferedAmount,
    directErrors: state.directErrors,
    directKeyframeRequests: state.directKeyframeRequests,
    directReconnecting: state.directReconnecting,
    directReconnectAttempts: state.directReconnectAttempts,
    directReconnects: state.directReconnects,
    directRecoveryState: state.directRecoveryState,
    directRecoveryStartedAt: state.directRecoveryStartedAt,
    directRecoveryElapsedMs: state.directRecoveryElapsedMs,
    directRecoveryLastDurationMs: state.directRecoveryLastDurationMs,
    directRecoveryTransientFailures: state.directRecoveryTransientFailures,
    directRecoveryCurrentAttempt: state.directRecoveryCurrentAttempt,
    directRecoveryLastOutcome: state.directRecoveryLastOutcome,
    directLastCloseCode: state.directLastCloseCode,
    directLastCloseWasClean: state.directLastCloseWasClean,
    lastPongAt: state.lastPongAt,
    receiverReceivedFrames: state.receiverReceivedFrames,
    receiverReceivedBytes: state.receiverReceivedBytes,
    receiverDecodedFrames: state.receiverDecodedFrames,
    receiverDroppedFrames: state.receiverDroppedFrames,
    startedAt: state.startedAt,
    stoppedAt: state.stoppedAt,
    stopReason: state.stopReason,
    error: state.error,
    events: state.events,
    stageRuns: state.stageRuns
  };

  const blob = new Blob([JSON.stringify(safeResult, null, 2)], {
    type: "application/json"
  });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `capture-probe-${new Date().toISOString().replaceAll(":", "-")}.json`;
  anchor.click();
  URL.revokeObjectURL(url);
}

function render(state) {
  document.body.dataset.mode = state.mode;
  document.body.dataset.stalled = String(Boolean(state.stalled));

  elements.status.textContent = getStatusText(state);
  elements.fps.textContent =
    Number.isFinite(state.fps) && state.mode !== "idle" ? state.fps.toFixed(1) : "—";
  elements.frames.textContent = Number(state.totalFrames || 0).toLocaleString("zh-CN");
  elements.encodingFps.textContent =
    Number.isFinite(state.encodingFps) && state.mode !== "idle"
      ? state.encodingFps.toFixed(1)
      : "—";
  elements.encodedFrames.textContent = Number(
    state.encodedFrames || 0
  ).toLocaleString("zh-CN");
  elements.bitrate.textContent =
    Number.isFinite(state.bitrateKbps) && state.mode !== "idle"
      ? Math.round(state.bitrateKbps).toLocaleString("zh-CN")
      : "—";
  elements.age.textContent =
    state.lastFrameAgeMs === null || state.lastFrameAgeMs === undefined
      ? "—"
      : Math.round(state.lastFrameAgeMs).toLocaleString("zh-CN");
  elements.stalls.textContent = Number(state.stallEvents || 0).toLocaleString("zh-CN");
  elements.droppedFrames.textContent = Number(
    state.droppedFrames || 0
  ).toLocaleString("zh-CN");
  elements.queueSize.textContent = Number(
    state.encodeQueueSize || 0
  ).toLocaleString("zh-CN");
  elements.encodeErrors.textContent = Number(
    state.encodeErrors || 0
  ).toLocaleString("zh-CN");
  elements.encoderConfig.textContent = formatEncoderConfig(state.encoderConfig);
  elements.directStatus.textContent = getDirectStatusText(state);
  elements.directSent.textContent = Number(
    state.directSentFrames || 0
  ).toLocaleString("zh-CN");
  elements.directReceived.textContent = Number(
    state.receiverDecodedFrames || 0
  ).toLocaleString("zh-CN");
  elements.directDropped.textContent = Number(
    (state.directDroppedFrames || 0) + (state.receiverDroppedFrames || 0)
  ).toLocaleString("zh-CN");
  elements.directErrors.textContent = Number(
    state.directErrors || 0
  ).toLocaleString("zh-CN");

  const canStop = ["starting", "capturing"].includes(state.mode);
  elements.stop.hidden = !canStop;
  elements.stop.disabled = state.mode === "starting";
  elements.reset.hidden = !["stopped", "error"].includes(state.mode);

  elements.error.hidden = !state.error;
  elements.error.textContent = state.error || "";

  for (const button of elements.stages) {
    if (button.dataset.stage === state.currentStage) {
      button.setAttribute("aria-current", "step");
    } else {
      button.removeAttribute("aria-current");
    }
    button.disabled = state.mode !== "capturing";
  }

  renderEvents(state.events || []);
}

function formatEncoderConfig(config) {
  if (!config) {
    return "等待 H.264 能力探测";
  }
  const bitrateMbps = Number(config.bitrate || 0) / 1_000_000;
  return [
    config.codec,
    `${config.width}×${config.height}`,
    `${config.framerate} fps`,
    `${bitrateMbps.toFixed(1)} Mbps`,
    config.avcFormat,
    config.hardwareAcceleration
  ]
    .filter(Boolean)
    .join(" · ");
}

function getDirectStatusText(state) {
  if (state.directConnected) {
    return "已连接";
  }
  if (state.directReconnecting) {
    return "正在恢复";
  }
  if (state.mode === "stopped" && Number(state.directSentFrames || 0) > 0) {
    return "已停止";
  }
  if (state.mode === "error") {
    return "失败";
  }
  return "等待";
}

function renderEvents(events) {
  if (events.length === 0) {
    elements.log.innerHTML = '<p class="empty-log">尚无事件</p>';
    return;
  }

  const fragment = document.createDocumentFragment();
  for (const event of events) {
    const row = document.createElement("div");
    row.className = "event-row";
    row.dataset.level = event.level;

    const time = document.createElement("span");
    time.textContent = new Date(event.at).toLocaleTimeString("zh-CN", {
      hour12: false,
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      fractionalSecondDigits: 3
    });
    const level = document.createElement("span");
    level.textContent = event.level;
    const message = document.createElement("span");
    message.textContent = event.message;
    row.append(time, level, message);
    fragment.append(row);
  }

  elements.log.replaceChildren(fragment);
  elements.log.scrollTop = elements.log.scrollHeight;
}

function getStatusText(state) {
  if (state.stalled) {
    return "检测到定格";
  }
  return {
    idle: "等待开始",
    starting: "正在启动",
    capturing: "正在捕获",
    stopping: "正在停止",
    stopped: "捕获已停止",
    error: "捕获失败"
  }[state.mode] ?? "状态未知";
}

function showLocalError(message) {
  elements.error.hidden = false;
  elements.error.textContent = message;
}

function createFallbackState() {
  return {
    mode: "idle",
    currentStage: "A",
    fps: 0,
    totalFrames: 0,
    lastFrameAgeMs: null,
    stallEvents: 0,
    stalled: false,
    encodingFps: 0,
    encodedFrames: 0,
    bitrateKbps: 0,
    droppedFrames: 0,
    encodeQueueSize: 0,
    encodeErrors: 0,
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
    error: null,
    events: [],
    stageRuns: []
  };
}
