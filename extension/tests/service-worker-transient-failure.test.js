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

async function waitForStoredFailure() {
  for (let index = 0; index < 20; index += 1) {
    const state = sessionValues.get(STATE_KEY);
    if (state?.mode === "error") return state;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  throw new Error("capture failure was not persisted");
}

test("service worker keeps AD1 transient while persisting only baseline failure", async () => {
  const baseline =
    "发送到平板失败：无法验证自动地址的私网归属，已拒绝发送凭据";
  const diagnostic =
    "AD1|B=1|Q=shape_parent|R=1|T=completed|I=1|C=0|S=open";
  const visibleFailure = `${baseline}（诊断码：${diagnostic}）`;
  assert.deepEqual(dispatch({
    target: "service-worker",
    type: "CAPTURE_FAILURE",
    error: visibleFailure,
    telemetry: null
  }), {
    keepChannelOpen: false,
    response: { ok: true }
  });

  const stored = await waitForStoredFailure();
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

  const monitorSender = {
    id: EXTENSION_ID,
    url: `${EXTENSION_ORIGIN}/monitor.html`
  };
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
});
