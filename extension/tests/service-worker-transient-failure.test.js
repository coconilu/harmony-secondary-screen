import test from "node:test";
import assert from "node:assert/strict";

const EXTENSION_ID = "abcdefghijklmnopabcdefghijklmnop";
const EXTENSION_ORIGIN = `chrome-extension://${EXTENSION_ID}`;
const OFFSCREEN_URL = `${EXTENSION_ORIGIN}/offscreen.html`;
const STATE_KEY = "captureProbeState";
const listeners = {};
const sessionValues = new Map();
const localValues = new Map([
  ["trustedReceiver", {
    senderId: "019fa3cf-75c7-7000-8000-000000000001",
    deviceId: "00112233445566778899aabbccddeeff",
    credential: "a".repeat(64),
    host: "harmony-web-companion.local",
    pairedAt: 1
  }],
  ["sourceEpoch", 0]
]);

let currentOffscreenContext = null;
let nextDocumentId = "document-a";
let lastOffscreenStart = null;

function event(name) {
  return {
    addListener(listener) {
      listeners[name] = listener;
    }
  };
}

function storageArea(values) {
  return {
    async get(key) {
      return { [key]: structuredClone(values.get(key)) };
    },
    async set(entries) {
      for (const [key, value] of Object.entries(entries)) {
        values.set(key, structuredClone(value));
      }
    },
    async remove(key) {
      values.delete(key);
    }
  };
}

globalThis.chrome = {
  action: {
    async setBadgeBackgroundColor() {},
    async setBadgeText() {},
    async setPopup() {},
    async setTitle() {}
  },
  offscreen: {
    async createDocument() {
      assert.equal(currentOffscreenContext, null);
      currentOffscreenContext = {
        contextType: "OFFSCREEN_DOCUMENT",
        documentId: nextDocumentId,
        documentUrl: OFFSCREEN_URL
      };
    },
    async closeDocument() {
      currentOffscreenContext = null;
    }
  },
  runtime: {
    id: EXTENSION_ID,
    getURL: (path) => `${EXTENSION_ORIGIN}/${path}`,
    getContexts: async () =>
      currentOffscreenContext === null
        ? []
        : [structuredClone(currentOffscreenContext)],
    async sendMessage(message) {
      assert.equal(message.target, "offscreen");
      if (message.type === "START_CAPTURE") {
        lastOffscreenStart = structuredClone(message);
        return { ok: true, telemetry: {} };
      }
      if (message.type === "STOP_CAPTURE") {
        return { ok: true, telemetry: {} };
      }
      throw new Error(`unexpected offscreen message: ${message.type}`);
    },
    onInstalled: event("installed"),
    onMessage: event("message"),
    onStartup: event("startup")
  },
  storage: {
    local: storageArea(localValues),
    session: storageArea(sessionValues)
  },
  tabCapture: {
    async getMediaStreamId() {
      return "stream-id";
    }
  },
  tabs: {
    async query() {
      return [{
        id: 7,
        url: "https://example.test/"
      }];
    }
  },
  webRequest: {
    onBeforeRequest: event("before"),
    onCompleted: event("completed"),
    onErrorOccurred: event("error"),
    onResponseStarted: event("response")
  }
};

await import("../service-worker.js");

function dispatch(message, sender = {}) {
  let response;
  const keepChannelOpen = listeners.message(
    message,
    sender,
    (value) => {
      response = value;
    }
  );
  return { keepChannelOpen, response };
}

async function dispatchAsync(message, sender = {}) {
  let resolveResponse;
  const responsePromise = new Promise((resolve) => {
    resolveResponse = resolve;
  });
  const keepChannelOpen = listeners.message(
    message,
    sender,
    resolveResponse
  );
  assert.equal(keepChannelOpen, true);
  return {
    keepChannelOpen,
    response: await responsePromise
  };
}

async function waitFor(predicate, errorMessage) {
  for (let index = 0; index < 100; index += 1) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  throw new Error(errorMessage);
}

async function waitForState(predicate) {
  await waitFor(
    () => predicate(sessionValues.get(STATE_KEY)),
    "expected service-worker state was not persisted"
  );
  return sessionValues.get(STATE_KEY);
}

async function waitForOffscreenClose() {
  await waitFor(
    () => currentOffscreenContext === null,
    "offscreen document was not closed"
  );
}

function setupSender() {
  return {
    id: EXTENSION_ID,
    url: `${EXTENSION_ORIGIN}/setup.html`
  };
}

function monitorSender() {
  return {
    id: EXTENSION_ID,
    url: `${EXTENSION_ORIGIN}/monitor.html`
  };
}

function offscreenSender(documentId, extra = {}) {
  return {
    id: EXTENSION_ID,
    origin: EXTENSION_ORIGIN,
    documentId,
    url: OFFSCREEN_URL,
    ...extra
  };
}

async function startCaptureSession(documentId) {
  nextDocumentId = documentId;
  lastOffscreenStart = null;
  const result = await dispatchAsync({
    target: "service-worker",
    type: "START_CAPTURE"
  }, setupSender());
  assert.equal(result.response.ok, true);
  assert.equal(currentOffscreenContext?.documentId, documentId);
  assert.match(
    lastOffscreenStart?.captureSessionId,
    /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/
  );
  await waitForState((state) => state?.mode === "capturing");
  return lastOffscreenStart.captureSessionId;
}

function captureMessage(type, captureSessionId, payload = {}) {
  return {
    target: "service-worker",
    type,
    captureSessionId,
    ...payload
  };
}

function takeTransient() {
  return dispatch({
    target: "service-worker",
    type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
  }, monitorSender()).response;
}

test("service worker binds capture messages to the active offscreen document lifecycle", async () => {
  const baseline =
    "发送到平板失败：无法验证自动地址的私网归属，已拒绝发送凭据";
  const diagnostic =
    "AD1|B=1|Q=shape_parent|R=1|T=completed|I=1|C=0|S=open";
  const sessionA = await startCaptureSession("document-a");
  const documentA = offscreenSender("document-a");
  const documentB = offscreenSender("document-b");

  const stateBeforeMismatches = structuredClone(sessionValues.get(STATE_KEY));
  for (const [message, expectedError] of [
    [
      captureMessage("CAPTURE_TELEMETRY", sessionA, {
        telemetry: { totalFrames: 999 }
      }),
      "拒绝非捕获页遥测"
    ],
    [
      captureMessage("CAPTURE_ENDED", sessionA, {
        reason: "forged",
        telemetry: { totalFrames: 999 }
      }),
      "拒绝非捕获页结束事件"
    ],
    [
      captureMessage("CAPTURE_FAILURE", sessionA, {
        error: "伪造失败",
        diagnosticCode: diagnostic,
        telemetry: { totalFrames: 999 }
      }),
      "拒绝非捕获页失败事件"
    ]
  ]) {
    assert.deepEqual(dispatch(message, documentB), {
      keepChannelOpen: false,
      response: { ok: false, error: expectedError }
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    assert.deepEqual(sessionValues.get(STATE_KEY), stateBeforeMismatches);
    assert.deepEqual(takeTransient(), { ok: true, error: null });
  }

  for (const sender of [
    offscreenSender("document-a", { url: `${EXTENSION_ORIGIN}/setup.html` }),
    offscreenSender("document-a", { id: "other-extension" }),
    offscreenSender("document-a", { origin: "https://example.test" }),
    offscreenSender(" ", {})
  ]) {
    assert.deepEqual(dispatch(
      captureMessage("CAPTURE_FAILURE", sessionA, {
        error: "伪造失败",
        diagnosticCode: diagnostic
      }),
      sender
    ).response, {
      ok: false,
      error: "拒绝非捕获页失败事件"
    });
    assert.deepEqual(sessionValues.get(STATE_KEY), stateBeforeMismatches);
    assert.deepEqual(takeTransient(), { ok: true, error: null });
  }

  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_TELEMETRY", sessionA, {
      telemetry: { totalFrames: 7 }
    }),
    { url: OFFSCREEN_URL }
  ).response, { ok: true });
  const edgeState = await waitForState(
    (state) => state?.mode === "capturing" && state.totalFrames === 7
  );
  assert.equal(edgeState.events.length, stateBeforeMismatches.events.length);

  const stopped = await dispatchAsync({
    target: "service-worker",
    type: "STOP_CAPTURE"
  }, monitorSender());
  assert.equal(stopped.response.ok, true);
  assert.equal(currentOffscreenContext, null);
  const stoppedState = await waitForState((state) => state?.mode === "stopped");

  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_TELEMETRY", sessionA, {
      telemetry: { totalFrames: 888 }
    }),
    { url: OFFSCREEN_URL }
  ).response, {
    ok: false,
    error: "拒绝非捕获页遥测"
  });
  assert.deepEqual(sessionValues.get(STATE_KEY), stoppedState);

  const sessionB = await startCaptureSession("document-b");
  assert.notEqual(sessionB, sessionA);
  const stateBeforeRotationAttacks =
    structuredClone(sessionValues.get(STATE_KEY));
  for (const [sender, captureSessionId] of [
    [documentA, sessionA],
    [documentA, sessionB],
    [{ url: OFFSCREEN_URL }, sessionA]
  ]) {
    assert.deepEqual(dispatch(
      captureMessage("CAPTURE_FAILURE", captureSessionId, {
        error: "旧文档伪造失败",
        diagnosticCode: diagnostic,
        telemetry: { totalFrames: 777 }
      }),
      sender
    ).response, {
      ok: false,
      error: "拒绝非捕获页失败事件"
    });
    assert.deepEqual(sessionValues.get(STATE_KEY), stateBeforeRotationAttacks);
    assert.deepEqual(takeTransient(), { ok: true, error: null });
  }

  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_FAILURE", sessionB, {
      error: baseline,
      diagnosticCode: diagnostic,
      telemetry: null
    }),
    documentB
  ).response, { ok: true });
  const stored = await waitForState((state) => state?.error === baseline);
  await waitForOffscreenClose();
  assert.equal(stored.events.at(-1).message, baseline);
  assert.equal(JSON.stringify(stored).includes("AD1"), false);
  assert.deepEqual(takeTransient(), {
    ok: true,
    error: `${baseline}（诊断码：${diagnostic}）`
  });
  assert.deepEqual(takeTransient(), { ok: true, error: null });

  for (const [index, untrustedText] of [
    `${baseline}（诊断码：${diagnostic}）（诊断码：${diagnostic}）`,
    `${baseline} 中间 ${diagnostic} 仍是普通错误`,
    `${baseline}（诊断码：（诊断码：${diagnostic}））`,
    `${baseline}（诊断码：AD1|B=9|Q=bound|R=1|T=completed|I=1|C=0|S=open）`
  ].entries()) {
    const session = await startCaptureSession(`malicious-document-${index}`);
    const previousEventCount =
      sessionValues.get(STATE_KEY)?.events?.length ?? 0;
    dispatch(
      captureMessage("CAPTURE_FAILURE", session, {
        error: untrustedText,
        telemetry: null
      }),
      offscreenSender(`malicious-document-${index}`)
    );
    const persisted = await waitForState(
      (state) =>
        state?.error === "捕获失败：错误信息包含无效诊断内容" &&
        state.events.length > previousEventCount
    );
    await waitForOffscreenClose();
    assert.equal(JSON.stringify(persisted).includes("AD1"), false);
    assert.equal(persisted.events.at(-1).message, persisted.error);
    assert.deepEqual(takeTransient(), { ok: true, error: null });
  }

  const dirtySession = await startCaptureSession("dirty-document");
  dispatch(
    captureMessage("CAPTURE_FAILURE", dirtySession, {
      error: `${baseline} AD1 不能作为结构化诊断`,
      diagnosticCode: diagnostic,
      telemetry: null
    }),
    offscreenSender("dirty-document")
  );
  const sanitizedState = await waitForState(
    (state) => state?.error === "捕获失败：错误信息包含无效诊断内容"
  );
  await waitForOffscreenClose();
  assert.equal(JSON.stringify(sanitizedState).includes("AD1"), false);
  const structuredTransient = takeTransient().error;
  assert.equal(
    structuredTransient,
    `捕获失败：错误信息包含无效诊断内容（诊断码：${diagnostic}）`
  );
  assert.equal(structuredTransient.match(/AD1/g)?.length, 1);

  const invalidSession = await startCaptureSession("invalid-code-document");
  dispatch(
    captureMessage("CAPTURE_FAILURE", invalidSession, {
      error: baseline,
      diagnosticCode: `${diagnostic}${diagnostic}`,
      telemetry: null
    }),
    offscreenSender("invalid-code-document")
  );
  await waitForState((state) => state?.error === baseline);
  await waitForOffscreenClose();
  assert.deepEqual(takeTransient(), { ok: true, error: null });

  const edgeSessionA = await startCaptureSession(undefined);
  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_TELEMETRY", edgeSessionA, {
      telemetry: { totalFrames: 11 }
    }),
    { url: OFFSCREEN_URL }
  ).response, { ok: true });
  await waitForState(
    (state) => state?.mode === "capturing" && state.totalFrames === 11
  );
  await dispatchAsync({
    target: "service-worker",
    type: "STOP_CAPTURE"
  }, monitorSender());

  const edgeSessionB = await startCaptureSession(undefined);
  assert.notEqual(edgeSessionB, edgeSessionA);
  const edgeRotationState = structuredClone(sessionValues.get(STATE_KEY));
  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_TELEMETRY", edgeSessionA, {
      telemetry: { totalFrames: 999 }
    }),
    { url: OFFSCREEN_URL }
  ).response, {
    ok: false,
    error: "拒绝非捕获页遥测"
  });
  assert.deepEqual(sessionValues.get(STATE_KEY), edgeRotationState);
  assert.deepEqual(takeTransient(), { ok: true, error: null });

  dispatch(
    captureMessage("CAPTURE_FAILURE", edgeSessionB, {
      error: baseline,
      telemetry: null
    }),
    { url: OFFSCREEN_URL }
  );
  await waitForState((state) => state?.error === baseline);
  await waitForOffscreenClose();

  const restartSessionA = await startCaptureSession("restart-document");
  const restartSessionB = await startCaptureSession("restart-document");
  assert.notEqual(restartSessionB, restartSessionA);
  const restartedState = structuredClone(sessionValues.get(STATE_KEY));
  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_ENDED", restartSessionA, {
      reason: "old_start",
      telemetry: { totalFrames: 1234 }
    }),
    offscreenSender("restart-document")
  ).response, {
    ok: false,
    error: "拒绝非捕获页结束事件"
  });
  assert.deepEqual(sessionValues.get(STATE_KEY), restartedState);
  assert.deepEqual(takeTransient(), { ok: true, error: null });

  const reset = await dispatchAsync({
    target: "service-worker",
    type: "RESET_PROBE"
  }, monitorSender());
  assert.equal(reset.response.ok, true);
  assert.equal(currentOffscreenContext, null);
  const resetState = await waitForState((state) => state?.mode === "idle");
  assert.deepEqual(dispatch(
    captureMessage("CAPTURE_FAILURE", restartSessionB, {
      error: "reset 后的旧文档失败",
      diagnosticCode: diagnostic,
      telemetry: { totalFrames: 5678 }
    }),
    offscreenSender("restart-document")
  ).response, {
    ok: false,
    error: "拒绝非捕获页失败事件"
  });
  assert.deepEqual(sessionValues.get(STATE_KEY), resetState);
  assert.deepEqual(takeTransient(), { ok: true, error: null });
});
