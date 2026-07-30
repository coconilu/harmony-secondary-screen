import test from "node:test";
import assert from "node:assert/strict";

const EXTENSION_ID = "abcdefghijklmnopabcdefghijklmnop";
const EXTENSION_ORIGIN = `chrome-extension://${EXTENSION_ID}`;
const STATE_KEY = "captureProbeState";
const TRUSTED = Object.freeze({
  senderId: "019fa3cf-75c7-7000-8000-000000000001",
  deviceId: "a".repeat(32),
  credential: "2".repeat(64),
  host: "harmony-web-companion.local",
  pairedAt: 123
});

test("offscreen to service-worker session and export keep start AD1 local", async () => {
  const previous = {
    chrome: globalThis.chrome,
    document: globalThis.document,
    window: globalThis.window,
    createObjectURL: URL.createObjectURL,
    revokeObjectURL: URL.revokeObjectURL
  };
  try {
    const offscreenFailures = [];
    for (const beginResponse of [null, {
      ok: false,
      error: "伪造 AD1|B=1|Q=bound|R=1|T=completed|I=1|C=0|S=open"
    }]) {
      offscreenFailures.push(await runOffscreenStartFailure(beginResponse));
    }
    assert.deepEqual(offscreenFailures, [
      "自动地址安全检查不可用，已拒绝建立 Receiver 连接",
      "无法启动自动地址安全检查，已拒绝建立 Receiver 连接"
    ]);
    assert.equal(JSON.stringify(offscreenFailures).includes("AD1"), false);

    const { sessionValues, dispatch } = await loadServiceWorker();
    for (const error of offscreenFailures) {
      assert.deepEqual(dispatch({
        target: "service-worker",
        type: "CAPTURE_FAILURE",
        error,
        telemetry: null
      }), {
        keepChannelOpen: false,
        response: { ok: true }
      });
      await waitForState(
        sessionValues,
        (state) => state?.error === error
      );
    }

    const stored = sessionValues.get(STATE_KEY);
    assert.equal(JSON.stringify(stored).includes("AD1"), false);
    assert.equal(stored.events.some(
      (entry) => String(entry.message).includes("AD1")
    ), false);

    const exported = await exportMonitorState(stored);
    assert.equal(exported.error, offscreenFailures.at(-1));
    assert.equal(JSON.stringify(exported).includes("AD1"), false);
    assert.equal(exported.events.some(
      (entry) => String(entry.message).includes("AD1")
    ), false);
  } finally {
    restoreGlobal("chrome", previous.chrome);
    restoreGlobal("document", previous.document);
    restoreGlobal("window", previous.window);
    URL.createObjectURL = previous.createObjectURL;
    URL.revokeObjectURL = previous.revokeObjectURL;
  }
});

async function runOffscreenStartFailure(beginResponse) {
  let onMessage;
  globalThis.chrome = {
    runtime: {
      onMessage: {
        addListener(listener) {
          onMessage = listener;
        }
      },
      ...(beginResponse === null
        ? {}
        : {
            async sendMessage(message) {
              if (message?.type === "BEGIN_DIRECT_OBSERVATION") {
                return beginResponse;
              }
              return { ok: true };
            }
          })
    }
  };
  await import(`../offscreen.js?boundary=${beginResponse === null ? 1 : 2}`);
  const response = await new Promise((resolve, reject) => {
    const keepChannelOpen = onMessage({
      target: "offscreen",
      type: "START_CAPTURE",
      streamId: "stream-id",
      directInfo: {
        trustedDevice: TRUSTED,
        sourceEpoch: 1
      }
    }, {}, resolve);
    if (!keepChannelOpen) reject(new Error("offscreen channel closed early"));
  });
  assert.equal(response.ok, false);
  assert.equal(response.error.includes("AD1"), false);
  return response.error;
}

async function loadServiceWorker() {
  const listeners = {};
  const sessionValues = new Map();
  const event = (name) => ({
    addListener(listener) {
      listeners[name] = listener;
    }
  });
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
  await import("../service-worker.js?boundary=1");
  return {
    sessionValues,
    dispatch(message) {
      let response;
      const keepChannelOpen = listeners.message(
        message,
        { url: `${EXTENSION_ORIGIN}/offscreen.html` },
        (value) => {
          response = value;
        }
      );
      return { keepChannelOpen, response };
    }
  };
}

async function waitForState(sessionValues, predicate) {
  for (let index = 0; index < 50; index += 1) {
    const state = sessionValues.get(STATE_KEY);
    if (predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  throw new Error("expected service-worker state was not persisted");
}

async function exportMonitorState(state) {
  let exportedBlob = null;
  const elements = new Map();
  const element = (selector) => {
    if (!elements.has(selector)) elements.set(selector, new FakeNode());
    return elements.get(selector);
  };
  globalThis.document = {
    body: { dataset: {} },
    querySelector: element,
    querySelectorAll() {
      return [];
    },
    createDocumentFragment() {
      return new FakeNode();
    },
    createElement() {
      return new FakeNode();
    }
  };
  globalThis.window = { close() {} };
  globalThis.chrome = {
    runtime: {
      async sendMessage() {
        return { ok: true };
      }
    },
    storage: {
      onChanged: {
        addListener() {}
      },
      session: {
        async get(key) {
          return { [key]: structuredClone(state) };
        }
      }
    }
  };
  URL.createObjectURL = (blob) => {
    exportedBlob = blob;
    return "blob:diagnostic-boundary";
  };
  URL.revokeObjectURL = () => {};
  await import("../monitor.js?boundary=1");
  await elements.get("#export-button").listeners.get("click")();
  assert.equal(exportedBlob instanceof Blob, true);
  return JSON.parse(await exportedBlob.text());
}

class FakeNode {
  constructor() {
    this.dataset = {};
    this.listeners = new Map();
    this.hidden = false;
    this.disabled = false;
    this.textContent = "";
    this.innerHTML = "";
    this.scrollHeight = 0;
    this.scrollTop = 0;
  }

  addEventListener(type, listener) {
    this.listeners.set(type, listener);
  }

  setAttribute() {}

  removeAttribute() {}

  append() {}

  replaceChildren() {}

  click() {}
}

function restoreGlobal(name, value) {
  if (value === undefined) {
    delete globalThis[name];
  } else {
    globalThis[name] = value;
  }
}
