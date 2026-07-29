import test from "node:test";
import assert from "node:assert/strict";

const EXTENSION_ID = "abcdefghijklmnopabcdefghijklmnop";
const EXTENSION_ORIGIN = `chrome-extension://${EXTENSION_ID}`;
const STATE_KEY = "captureProbeState";
const listeners = {};
const sessionValues = new Map();

function event(name) {
  return {
    addListener(listener) {
      listeners[name] = listener;
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
    async closeDocument() {}
  },
  runtime: {
    id: EXTENSION_ID,
    getURL: (path) => `${EXTENSION_ORIGIN}/${path}`,
    getContexts: async () => [],
    onInstalled: event("installed"),
    onMessage: event("message"),
    onStartup: event("startup")
  },
  storage: {
    session: {
      async get(key) {
        return { [key]: structuredClone(sessionValues.get(key)) };
      },
      async set(entries) {
        for (const [key, value] of Object.entries(entries)) {
          sessionValues.set(key, structuredClone(value));
        }
      }
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

async function waitForState(predicate) {
  for (let index = 0; index < 50; index += 1) {
    const state = sessionValues.get(STATE_KEY);
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  throw new Error("expected service-worker state was not persisted");
}

test("service worker accepts structured AD1 only from the exact offscreen document", async () => {
  const baseline =
    "发送到平板失败：无法验证自动地址的私网归属，已拒绝发送凭据";
  const diagnostic =
    "AD1|B=1|Q=shape_parent|R=1|T=completed|I=1|C=0|S=open";
  const visibleFailure = `${baseline}（诊断码：${diagnostic}）`;
  const offscreenSender = {
    url: `${EXTENSION_ORIGIN}/offscreen.html`
  };
  const monitorSender = {
    id: EXTENSION_ID,
    url: `${EXTENSION_ORIGIN}/monitor.html`
  };

  for (const sender of [
    { url: `${EXTENSION_ORIGIN}/setup.html` },
    { url: `${EXTENSION_ORIGIN}/monitor.html` },
    {
      id: "other-extension",
      url: `${EXTENSION_ORIGIN}/offscreen.html`
    },
    {
      origin: "https://example.test",
      url: `${EXTENSION_ORIGIN}/offscreen.html`
    },
    {
      documentId: " ",
      url: `${EXTENSION_ORIGIN}/offscreen.html`
    },
    { url: "https://example.test/offscreen.html" }
  ]) {
    assert.deepEqual(dispatch({
      target: "service-worker",
      type: "CAPTURE_FAILURE",
      error: baseline,
      diagnosticCode: diagnostic,
      telemetry: null
    }, sender), {
      keepChannelOpen: false,
      response: { ok: false, error: "拒绝非捕获页失败事件" }
    });
  }
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(sessionValues.has(STATE_KEY), false);

  assert.deepEqual(dispatch({
    target: "service-worker",
    type: "CAPTURE_TELEMETRY",
    telemetry: { totalFrames: 999 }
  }, { url: `${EXTENSION_ORIGIN}/setup.html` }).response, {
    ok: false,
    error: "拒绝非捕获页遥测"
  });
  assert.deepEqual(dispatch({
    target: "service-worker",
    type: "CAPTURE_ENDED",
    reason: "forged"
  }, { url: `${EXTENSION_ORIGIN}/monitor.html` }).response, {
    ok: false,
    error: "拒绝非捕获页结束事件"
  });

  assert.deepEqual(dispatch({
    target: "service-worker",
    type: "CAPTURE_FAILURE",
    error: baseline,
    diagnosticCode: diagnostic,
    telemetry: null
  }, offscreenSender), {
    keepChannelOpen: false,
    response: { ok: true }
  });

  const stored = await waitForState((state) => state?.error === baseline);
  assert.equal(stored.error, baseline);
  assert.equal(stored.events.at(-1).message, baseline);
  assert.equal(JSON.stringify(stored).includes("AD1"), false);
  assert.equal(JSON.stringify(stored).includes("诊断码"), false);

  const rejectedTake = dispatch({
    target: "service-worker",
    type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
  }, {
    id: EXTENSION_ID,
    url: `${EXTENSION_ORIGIN}/setup.html`
  });
  assert.deepEqual(rejectedTake.response, { ok: true, error: null });

  const firstOpen = dispatch({
    target: "service-worker",
    type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
  }, monitorSender);
  assert.deepEqual(firstOpen.response, {
    ok: true,
    error: visibleFailure
  });

  const reopened = dispatch({
    target: "service-worker",
    type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
  }, monitorSender);
  assert.deepEqual(reopened.response, { ok: true, error: null });
  assert.equal(JSON.stringify(sessionValues.get(STATE_KEY)).includes("AD1"), false);

  for (const untrustedText of [
    `${baseline}（诊断码：${diagnostic}）（诊断码：${diagnostic}）`,
    `${baseline} 中间 ${diagnostic} 仍是普通错误`,
    `${baseline}（诊断码：（诊断码：${diagnostic}））`,
    `${baseline}（诊断码：AD1|B=9|Q=bound|R=1|T=completed|I=1|C=0|S=open）`
  ]) {
    const previousEventCount =
      sessionValues.get(STATE_KEY)?.events?.length ?? 0;
    dispatch({
      target: "service-worker",
      type: "CAPTURE_FAILURE",
      error: untrustedText,
      telemetry: null
    }, offscreenSender);
    const persisted = await waitForState(
      (state) =>
        state?.error === "捕获失败：错误信息包含无效诊断内容" &&
        state.events.length > previousEventCount
    );
    assert.equal(JSON.stringify(persisted).includes("AD1"), false);
    assert.equal(persisted.events.at(-1).message, persisted.error);
    assert.deepEqual(dispatch({
      target: "service-worker",
      type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
    }, monitorSender).response, { ok: true, error: null });
  }

  const dirtyBaseline = `${baseline} AD1 不能作为结构化诊断`;
  const sanitizedBaseline = "捕获失败：错误信息包含无效诊断内容";
  const previousEventCount =
    sessionValues.get(STATE_KEY)?.events?.length ?? 0;
  dispatch({
    target: "service-worker",
    type: "CAPTURE_FAILURE",
    error: dirtyBaseline,
    diagnosticCode: diagnostic,
    telemetry: null
  }, {
    id: EXTENSION_ID,
    origin: `${EXTENSION_ORIGIN}/`,
    documentId: "offscreen-document",
    url: `${EXTENSION_ORIGIN}/offscreen.html`
  });
  const sanitizedState = await waitForState(
    (state) =>
      state?.error === sanitizedBaseline &&
      state.events.length > previousEventCount
  );
  assert.equal(JSON.stringify(sanitizedState).includes("AD1"), false);
  const structuredTransient = dispatch({
    target: "service-worker",
    type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
  }, monitorSender).response.error;
  assert.equal(
    structuredTransient,
    `${sanitizedBaseline}（诊断码：${diagnostic}）`
  );
  assert.equal(structuredTransient.match(/AD1/g)?.length, 1);

  dispatch({
    target: "service-worker",
    type: "CAPTURE_FAILURE",
    error: baseline,
    diagnosticCode: `${diagnostic}${diagnostic}`,
    telemetry: null
  }, offscreenSender);
  await waitForState((state) => state?.error === baseline);
  assert.deepEqual(dispatch({
    target: "service-worker",
    type: "TAKE_TRANSIENT_CAPTURE_FAILURE"
  }, monitorSender).response, { ok: true, error: null });
});
