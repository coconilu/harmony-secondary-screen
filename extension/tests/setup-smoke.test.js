import test from "node:test";
import assert from "node:assert/strict";

const TRUSTED = {
  senderId: "019fa3cf-75c7-7000-8000-000000000001",
  deviceId: "00112233445566778899aabbccddeeff",
  credential: "a".repeat(64),
  host: "192.168.1.8",
  pairedAt: 123
};

class FakeElement {
  constructor({ hidden = false, value = "" } = {}) {
    this.hidden = hidden;
    this.value = value;
    this.disabled = false;
    this.textContent = "";
    this.width = 224;
    this.height = 224;
    this.listeners = new Map();
  }

  addEventListener(type, listener) {
    this.listeners.set(type, listener);
  }

  getContext() {
    return {
      fillStyle: "",
      fillRect() {}
    };
  }
}

function createPopupEnvironment({
  trusted = null,
  origins = [],
  removeResult = true
} = {}) {
  const values = new Map();
  if (trusted) values.set("trustedReceiver", trusted);
  const grantedOrigins = new Set(origins);
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
      local: {
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
      }
    },
    permissions: {
      async contains({ origins: queried }) {
        return queried.every((origin) => grantedOrigins.has(origin));
      },
      async request({ origins: requested }) {
        requested.forEach((origin) => grantedOrigins.add(origin));
        return true;
      },
      async getAll() {
        return { origins: [...grantedOrigins] };
      },
      async remove({ origins: removed }) {
        if (!removeResult) return false;
        removed.forEach((origin) => grantedOrigins.delete(origin));
        return true;
      }
    },
    runtime: {
      async sendMessage() {
        return { ok: true };
      }
    }
  };
  return { chrome, document, elements, grantedOrigins, values };
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
    /在平板输入短码 \d{6}/
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
