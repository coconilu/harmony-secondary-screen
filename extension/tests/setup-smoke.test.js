import test from "node:test";
import assert from "node:assert/strict";

import { createPairingAuthorization } from "../direct-protocol.js";
import {
  beginDirectTransportObservation,
  describeDirectTransportFailure,
  DirectRequestObserver,
  DirectTransportError
} from "../direct-network-diagnostics.js";

const TRUSTED = {
  senderId: "019fa3cf-75c7-7000-8000-000000000001",
  deviceId: "00112233445566778899aabbccddeeff",
  credential: "a".repeat(64),
  host: "192.168.1.8",
  pairedAt: 123
};

function createAutomaticPairingFailure() {
  const url = "ws://harmony-web-companion.local:44000/direct";
  const initiator =
    "chrome-extension://abcdefghijklmnopabcdefghijklmnop";
  const context = {
    documentId: "setup-document",
    initiator,
    page: "setup.html"
  };
  const details = {
    requestId: "pairing-request",
    url,
    type: "websocket",
    tabId: -1,
    frameId: 0,
    parentFrameId: -1,
    documentId: context.documentId,
    initiator
  };
  const observer = new DirectRequestObserver();
  const attemptId = observer.begin(url, context);
  observer.observeBefore(details);
  observer.observeTerminal(details);
  return new DirectTransportError(describeDirectTransportFailure({
    host: "harmony-web-companion.local",
    ...observer.finish(attemptId, "open", context)
  }));
}

async function captureObservationStartFailure(runtime) {
  const previousChrome = globalThis.chrome;
  if (runtime === null) {
    delete globalThis.chrome;
  } else {
    globalThis.chrome = { runtime };
  }
  try {
    await beginDirectTransportObservation(
      "ws://harmony-web-companion.local:44000/direct"
    );
    assert.fail("expected observation start to fail");
  } catch (error) {
    return error;
  } finally {
    if (previousChrome === undefined) {
      delete globalThis.chrome;
    } else {
      globalThis.chrome = previousChrome;
    }
  }
}

class FakeElement {
  constructor({ hidden = false, value = "" } = {}) {
    this.hidden = hidden;
    this.value = value;
    this.disabled = false;
    this.textContent = "";
    this.width = 224;
    this.height = 224;
    this.listeners = new Map();
    this.drawOperations = [];
  }

  addEventListener(type, listener) {
    this.listeners.set(type, listener);
  }

  getContext() {
    let fillStyle = "";
    return {
      get fillStyle() {
        return fillStyle;
      },
      set fillStyle(value) {
        fillStyle = value;
      },
      fillRect: (...values) => {
        this.drawOperations.push([fillStyle, ...values]);
      }
    };
  }
}

function createPopupEnvironment({
  trusted = null,
  origins = [],
  removeResult = true,
  sharedState = null,
  permissionRequest = null
} = {}) {
  const state = sharedState ?? {
    localValues: new Map(),
    sessionValues: new Map(),
    grantedOrigins: new Set(origins),
    requestCalls: []
  };
  if (trusted) state.localValues.set("trustedReceiver", trusted);
  const elements = new Map([
    ["#pairing-form", new FakeElement()],
    ["#paired-panel", new FakeElement({ hidden: true })],
    ["#receiver-address", new FakeElement({
      value: "harmony-web-companion.local"
    })],
    ["#pairing-qr", new FakeElement()],
    ["#paired-address", new FakeElement()],
    ["#paired-host", new FakeElement()],
    ["#short-code", new FakeElement()],
    ["#pair-button", new FakeElement()],
    ["#refresh-button", new FakeElement()],
    ["#start-button", new FakeElement()],
    ["#forget-button", new FakeElement()],
    ["#update-host-button", new FakeElement()],
    ["#error-message", new FakeElement({ hidden: true })]
  ]);

  const document = {
    querySelector(selector) {
      const element = elements.get(selector);
      if (!element) throw new Error(`unknown selector ${selector}`);
      return element;
    }
  };
  const chrome = {
    storage: {
      local: createStorageArea(state.localValues),
      session: createStorageArea(state.sessionValues)
    },
    permissions: {
      async contains({ origins: queried }) {
        return queried.every((origin) => state.grantedOrigins.has(origin));
      },
      async request({ origins: requested }) {
        state.requestCalls.push(...requested);
        if (permissionRequest) {
          return permissionRequest({ requested, state });
        }
        requested.forEach((origin) => state.grantedOrigins.add(origin));
        return true;
      },
      async getAll() {
        return { origins: [...state.grantedOrigins] };
      },
      async remove({ origins: removed }) {
        if (!removeResult) return false;
        removed.forEach((origin) => state.grantedOrigins.delete(origin));
        return true;
      }
    },
    runtime: {
      async sendMessage() {
        return { ok: true };
      }
    }
  };
  return {
    chrome,
    document,
    elements,
    grantedOrigins: state.grantedOrigins,
    sessionValues: state.sessionValues,
    state,
    values: state.localValues
  };
}

function createStorageArea(values) {
  return {
    async get(key) {
      return { [key]: values.get(key) };
    },
    async set(entries) {
      for (const [key, value] of Object.entries(entries)) {
        values.set(key, value);
      }
    },
    async remove(key) {
      values.delete(key);
    }
  };
}

let importSequence = 0;

async function loadSetupModule(context, environment) {
  const previousChrome = globalThis.chrome;
  const previousDocument = globalThis.document;
  globalThis.chrome = environment.chrome;
  globalThis.document = environment.document;
  context.after(() => {
    globalThis.chrome = previousChrome;
    globalThis.document = previousDocument;
  });
  const url = new URL("../setup.js", import.meta.url);
  url.searchParams.set("smoke", String(++importSequence));
  const module = await import(url);
  await module.setupReady;
  return module;
}

test("fresh install renders the pairing form and initializes a QR code", async (context) => {
  const environment = createPopupEnvironment();
  await loadSetupModule(context, environment);
  assert.equal(environment.elements.get("#pairing-form").hidden, false);
  assert.equal(environment.elements.get("#paired-panel").hidden, true);
  assert.equal(
    environment.elements.get("#paired-host").value,
    "harmony-web-companion.local"
  );
  assert.match(
    environment.elements.get("#short-code").textContent,
    /在平板输入 6 位连接码 \d{6}/
  );
  assert.equal(environment.elements.get("#error-message").hidden, true);
});

test("forgetting a device renders the unpaired state without a reference error", async (context) => {
  const origin = "http://192.168.1.8/*";
  const environment = createPopupEnvironment({
    trusted: TRUSTED,
    origins: [origin]
  });
  const setup = await loadSetupModule(context, environment);
  await setup.forgetPairing();
  assert.equal(environment.values.has("trustedReceiver"), false);
  assert.equal(environment.grantedOrigins.has(origin), false);
  assert.equal(environment.elements.get("#pairing-form").hidden, false);
  assert.equal(
    environment.elements.get("#paired-host").value,
    "harmony-web-companion.local"
  );
  assert.equal(environment.elements.get("#error-message").hidden, true);
});

test("load keeps permission cleanup failure visible for an unpaired user", async (context) => {
  const origin = "http://192.168.1.8/*";
  const environment = createPopupEnvironment({
    origins: [origin],
    removeResult: false
  });
  await loadSetupModule(context, environment);
  assert.equal(environment.grantedOrigins.has(origin), true);
  assert.equal(environment.elements.get("#error-message").hidden, false);
  assert.match(
    environment.elements.get("#error-message").textContent,
    /无法撤销/
  );
});

test("forget keeps permissions.remove false visible while returning to pairing", async (context) => {
  const origin = "http://192.168.1.8/*";
  const environment = createPopupEnvironment({
    trusted: TRUSTED,
    origins: [origin],
    removeResult: false
  });
  const setup = await loadSetupModule(context, environment);
  await setup.forgetPairing();
  assert.equal(environment.values.has("trustedReceiver"), false);
  assert.equal(environment.grantedOrigins.has(origin), true);
  assert.equal(environment.elements.get("#pairing-form").hidden, false);
  assert.equal(environment.elements.get("#error-message").hidden, false);
  assert.match(
    environment.elements.get("#error-message").textContent,
    /无法撤销/
  );
});

test("automatic pairing shows AD1 only in the current setup popup", async (context) => {
  const firstPopup = createPopupEnvironment();
  const setup = await loadSetupModule(context, firstPopup);
  const failure = createAutomaticPairingFailure();
  await setup.completePairing({
    async pair() {
      throw failure;
    }
  });
  assert.equal(
    firstPopup.elements.get("#error-message").textContent,
    `${failure.message}（诊断码：${failure.diagnosticCode}）`
  );
  assert.equal(firstPopup.elements.get("#error-message").hidden, false);
  assert.equal(
    JSON.stringify([...firstPopup.sessionValues.entries()]).includes("AD1"),
    false
  );
  assert.equal(
    JSON.stringify([...firstPopup.values.entries()]).includes("AD1"),
    false
  );

  const rebuiltPopup = createPopupEnvironment({
    sharedState: firstPopup.state
  });
  await loadSetupModule(context, rebuiltPopup);
  assert.equal(rebuiltPopup.elements.get("#error-message").hidden, true);
  assert.equal(
    rebuiltPopup.elements.get("#error-message").textContent.includes("AD1"),
    false
  );
  assert.equal(
    JSON.stringify([...rebuiltPopup.sessionValues.entries()]).includes("AD1"),
    false
  );
  assert.equal(
    JSON.stringify([...rebuiltPopup.values.entries()]).includes("AD1"),
    false
  );
});

test("setup alone reveals structured start diagnostics for both failure paths", async (context) => {
  const failures = [
    await captureObservationStartFailure(null),
    await captureObservationStartFailure({
      async sendMessage() {
        return {
          ok: false,
          error:
            "伪造 AD1|B=1|Q=bound|R=1|T=completed|I=1|C=0|S=open"
        };
      }
    })
  ];
  const environment = createPopupEnvironment();
  const setup = await loadSetupModule(context, environment);

  for (const failure of failures) {
    assert.equal(failure instanceof DirectTransportError, true);
    assert.equal(failure.message.includes("AD1"), false);
    await setup.completePairing({
      async pair() {
        throw failure;
      }
    });
    assert.equal(
      environment.elements.get("#error-message").textContent,
      `${failure.message}（诊断码：${failure.diagnosticCode}）`
    );
    assert.equal(
      JSON.stringify([...environment.sessionValues.entries()]).includes("AD1"),
      false
    );
    assert.equal(
      JSON.stringify([...environment.values.entries()]).includes("AD1"),
      false
    );
  }
});

test("ordinary, forged, manual and successful pairing paths show no AD1", async (context) => {
  const environment = createPopupEnvironment();
  const setup = await loadSetupModule(context, environment);
  const diagnostic = createAutomaticPairingFailure().diagnosticCode;
  for (const message of [
    `普通错误 ${diagnostic}`,
    `重复 ${diagnostic}${diagnostic}`,
    `嵌套（诊断码：（诊断码：${diagnostic}））`
  ]) {
    await setup.completePairing({
      async pair() {
        throw new Error(message);
      }
    });
    assert.equal(
      environment.elements.get("#error-message").textContent.includes("AD1"),
      false
    );
  }

  environment.elements.get("#receiver-address").value = "192.168.1.8";
  await setup.completePairing({
    async pair() {
      throw createAutomaticPairingFailure();
    }
  });
  assert.equal(
    environment.elements.get("#error-message").textContent.includes("AD1"),
    false
  );

  const manualFailure = new DirectTransportError(
    describeDirectTransportFailure({
      host: "192.168.1.8",
      socketOutcome: "error"
    })
  );
  await setup.completePairing({
    async pair() {
      throw manualFailure;
    }
  });
  assert.equal(
    environment.elements.get("#error-message").textContent,
    manualFailure.message
  );
  assert.equal(
    environment.elements.get("#error-message").textContent.includes("AD1"),
    false
  );

  environment.elements.get("#receiver-address").value =
    "harmony-web-companion.local";
  await setup.completePairing({
    async pair() {
      return {
        ...TRUSTED,
        host: "harmony-web-companion.local"
      };
    }
  });
  assert.equal(environment.elements.get("#error-message").hidden, true);
  assert.equal(
    JSON.stringify([...environment.sessionValues.entries()]).includes("AD1"),
    false
  );
  assert.equal(
    JSON.stringify([...environment.values.entries()]).includes("AD1"),
    false
  );
});

test("permission popup interruption restores the same pending pairing and finishes without a second prompt", async (context) => {
  let markPermissionRequested;
  const permissionRequested = new Promise((resolve) => {
    markPermissionRequested = resolve;
  });
  const firstEnvironment = createPopupEnvironment({
    permissionRequest({ requested, state }) {
      requested.forEach((origin) => state.grantedOrigins.add(origin));
      markPermissionRequested();
      return new Promise(() => {});
    }
  });
  const firstSetup = await loadSetupModule(context, firstEnvironment);
  firstEnvironment.elements.get("#receiver-address").value = "192.168.3.112";
  await firstSetup.updatePendingHost();
  const firstPending = structuredClone(
    firstEnvironment.sessionValues.get("pendingPairing")
  );
  const firstShortCode =
    firstEnvironment.elements.get("#short-code").textContent;
  const firstQr = structuredClone(
    firstEnvironment.elements.get("#pairing-qr").drawOperations
  );

  void firstSetup.completePairing({
    pair() {
      throw new Error("pairing must not start before permission returns");
    }
  });
  await permissionRequested;
  assert.equal(firstEnvironment.state.requestCalls.length, 1);
  assert.deepEqual(
    firstEnvironment.sessionValues.get("pendingPairing"),
    firstPending
  );
  assert.equal(firstEnvironment.values.has("pendingPairing"), false);

  const secondEnvironment = createPopupEnvironment({
    sharedState: firstEnvironment.state
  });
  const secondSetup = await loadSetupModule(context, secondEnvironment);
  assert.equal(
    secondEnvironment.elements.get("#receiver-address").value,
    "192.168.3.112"
  );
  assert.equal(
    secondEnvironment.elements.get("#short-code").textContent,
    firstShortCode
  );
  assert.deepEqual(
    secondEnvironment.elements.get("#pairing-qr").drawOperations,
    firstQr
  );
  assert.match(
    secondEnvironment.elements.get("#pair-button").textContent,
    /继续连接平板/
  );
  assert.equal(secondEnvironment.state.requestCalls.length, 1);

  await secondSetup.completePairing({
    async pair({ host, authorization, senderId }) {
      assert.equal(host, "192.168.3.112");
      assert.equal(
        authorization.sessionId,
        firstPending.authorization.sessionId
      );
      assert.equal(authorization.token, firstPending.authorization.token);
      return {
        ...TRUSTED,
        host,
        senderId
      };
    }
  });
  assert.equal(secondEnvironment.state.requestCalls.length, 1);
  assert.equal(secondEnvironment.sessionValues.has("pendingPairing"), false);
  assert.equal(secondEnvironment.values.has("trustedReceiver"), true);
  assert.equal(secondEnvironment.values.has("pendingPairing"), false);
  assert.equal(
    JSON.stringify([...secondEnvironment.values.entries()])
      .includes(firstPending.authorization.token),
    false
  );
});

test("expired pending state is replaced with a fresh authorization", async (context) => {
  const expired = createPairingAuthorization(Date.now() - 120_000);
  const environment = createPopupEnvironment();
  environment.sessionValues.set("pendingPairing", {
    host: "192.168.3.112",
    authorization: {
      sessionId: expired.sessionId,
      token: expired.token,
      shortCode: expired.shortCode,
      expiresAt: expired.expiresAt
    }
  });
  await loadSetupModule(context, environment);
  const replacement = environment.sessionValues.get("pendingPairing");
  assert.notEqual(replacement.authorization.token, expired.token);
  assert.ok(replacement.authorization.expiresAt > Date.now());
});

test("manual QR refresh replaces the pending authorization in session storage", async (context) => {
  const environment = createPopupEnvironment();
  const setup = await loadSetupModule(context, environment);
  const previous = structuredClone(
    environment.sessionValues.get("pendingPairing")
  );
  await setup.refreshAuthorization();
  const refreshed = environment.sessionValues.get("pendingPairing");
  assert.notEqual(
    refreshed.authorization.sessionId,
    previous.authorization.sessionId
  );
  assert.notEqual(refreshed.authorization.token, previous.authorization.token);
  assert.equal(environment.values.has("pendingPairing"), false);
});
