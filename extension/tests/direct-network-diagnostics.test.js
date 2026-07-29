import test from "node:test";
import assert from "node:assert/strict";

import {
  beginDirectTransportObservation,
  classifyObservedAddress,
  createDirectObservationContext,
  describeDirectTransportFailure,
  DirectRequestObserver,
  finishDirectTransportObservation,
  installDirectRequestObserver
} from "../direct-network-diagnostics.js";

const EXTENSION_ID = "abcdefghijklmnopabcdefghijklmnop";
const EXTENSION_ORIGIN = `chrome-extension://${EXTENSION_ID}`;
const CONTEXT_A = Object.freeze({
  documentId: "document-a",
  initiator: EXTENSION_ORIGIN,
  page: "setup.html"
});
const CONTEXT_B = Object.freeze({
  documentId: "document-b",
  initiator: EXTENSION_ORIGIN,
  page: "setup.html"
});
const CONTEXT_NO_DOCUMENT = Object.freeze({
  documentId: null,
  initiator: EXTENSION_ORIGIN,
  page: "setup.html"
});

function requestDetails(url, requestId, context = CONTEXT_A, extra = {}) {
  return {
    url,
    requestId,
    documentId: context.documentId,
    initiator: context.initiator,
    ...extra
  };
}

test("classifies only RFC1918 and IPv4 link-local as trusted", () => {
  for (const address of [
    "10.1.2.3",
    "172.16.0.1",
    "172.31.255.254",
    "192.168.4.8",
    "169.254.9.1"
  ]) {
    assert.equal(classifyObservedAddress(address), "private_ipv4");
  }
  for (const address of [
    "0.0.0.0",
    "127.0.0.1",
    "172.32.0.1",
    "192.0.2.1",
    "224.0.0.251",
    "2001:db8::1",
    "invalid"
  ]) {
    assert.equal(classifyObservedAddress(address), "non_private");
  }
});

test("separates automatic resolution, unsafe address, reachability and auth phases", () => {
  const host = "harmony-web-companion.local";
  assert.equal(describeDirectTransportFailure({
    host,
    socketOutcome: "error",
    observedAddressClass: "unresolved"
  }).code, "automatic_address_resolution_failed");
  assert.equal(describeDirectTransportFailure({
    host,
    socketOutcome: "open",
    observedAddressClass: "non_private"
  }).code, "automatic_address_not_private");
  assert.equal(describeDirectTransportFailure({
    host,
    socketOutcome: "error",
    observedAddressClass: "private_ipv4"
  }).code, "automatic_address_unreachable");
  assert.equal(describeDirectTransportFailure({
    host,
    socketOutcome: "timeout",
    observedAddressClass: "private_ipv4"
  }).code, "automatic_address_connection_timeout");
  assert.equal(describeDirectTransportFailure({
    host: "192.168.1.8",
    socketOutcome: "error"
  }).code, "manual_address_unreachable");
});

test("never allows credentials before a private automatic address is observed", () => {
  for (const observedAddressClass of ["unresolved", "non_private"]) {
    assert.equal(describeDirectTransportFailure({
      host: "harmony-web-companion.local",
      socketOutcome: "open",
      observedAddressClass
    }).allowAuthentication, false);
  }
  assert.equal(describeDirectTransportFailure({
    host: "harmony-web-companion.local",
    socketOutcome: "open",
    observedAddressClass: "private_ipv4"
  }).allowAuthentication, true);
});

test("missing runtime observation fails closed for automatic host", async () => {
  const previousChrome = globalThis.chrome;
  delete globalThis.chrome;
  try {
    await assert.rejects(
      beginDirectTransportObservation(
        "ws://harmony-web-companion.local:44000/direct"
      ),
      /安全检查不可用/
    );
    assert.deepEqual(
      await finishDirectTransportObservation(
        null,
        "harmony-web-companion.local",
        "open"
      ),
      {
        code: "automatic_address_unverified",
        message:
          "无法验证自动地址的私网归属，已拒绝发送凭据；请改用平板显示的数字 IPv4",
        allowAuthentication: false,
        recoverable: false
      }
    );
    assert.equal(
      (await finishDirectTransportObservation(
        null,
        "192.168.1.8",
        "open"
      )).allowAuthentication,
      true
    );
  } finally {
    if (previousChrome === undefined) {
      delete globalThis.chrome;
    } else {
      globalThis.chrome = previousChrome;
    }
  }
});

test("correlates one WebSocket request without persisting its raw address", async () => {
  let now = 1_000;
  const observer = new DirectRequestObserver({ now: () => now });
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-1"));
  observer.observeTerminal(requestDetails(
    url,
    "request-1",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));
  assert.deepEqual(
    await observer.finishWhenReady(attemptId, "open", 100, CONTEXT_A),
    {
      observedAddressClass: "private_ipv4",
      socketOutcome: "open"
    }
  );

  const expired = observer.begin(url, CONTEXT_A);
  now += 20_000;
  assert.deepEqual(observer.finish(expired, "error", CONTEXT_A), {
    observedAddressClass: "unresolved",
    socketOutcome: "error"
  });
});

test("validates setup and offscreen runtime document senders", () => {
  const runtimeApi = {
    id: EXTENSION_ID,
    getURL: (path) => `${EXTENSION_ORIGIN}/${path}`
  };
  for (const page of ["setup.html", "offscreen.html"]) {
    assert.deepEqual(createDirectObservationContext({
      id: EXTENSION_ID,
      documentId: `document-${page}`,
      origin: EXTENSION_ORIGIN,
      url: runtimeApi.getURL(page)
    }, runtimeApi), {
      documentId: `document-${page}`,
      initiator: EXTENSION_ORIGIN,
      page
    });
  }
  assert.deepEqual(createDirectObservationContext({
    id: EXTENSION_ID,
    url: runtimeApi.getURL("setup.html")
  }, runtimeApi), {
    documentId: null,
    initiator: EXTENSION_ORIGIN,
    page: "setup.html"
  });
  assert.deepEqual(createDirectObservationContext({
    origin: `${EXTENSION_ORIGIN}/`,
    url: runtimeApi.getURL("setup.html")
  }, runtimeApi), {
    documentId: null,
    initiator: EXTENSION_ORIGIN,
    page: "setup.html"
  });
  assert.throws(
    () => createDirectObservationContext({
      id: EXTENSION_ID
    }, runtimeApi),
    /缺少扩展页面 URL/
  );
  for (const sender of [
    {
      id: "other-extension",
      documentId: "document-other",
      origin: EXTENSION_ORIGIN,
      url: runtimeApi.getURL("setup.html")
    },
    {
      id: EXTENSION_ID,
      documentId: "document-monitor",
      origin: EXTENSION_ORIGIN,
      url: runtimeApi.getURL("monitor.html")
    },
    {
      id: EXTENSION_ID,
      documentId: "document-wrong-origin",
      origin: "https://example.test",
      url: runtimeApi.getURL("setup.html")
    }
  ]) {
    assert.throws(
      () => createDirectObservationContext(sender, runtimeApi),
      /来源扩展不匹配|只接受扩展配对页或捕获页|来源 origin 不匹配/
    );
  }
});

test("waits for terminal and merges a late conflicting address", async () => {
  const observer = new DirectRequestObserver();
  const listeners = new Map();
  const event = (name) => ({
    addListener(callback) {
      listeners.set(name, callback);
    }
  });
  installDirectRequestObserver(observer, {
    onBeforeRequest: event("before"),
    onResponseStarted: event("response"),
    onCompleted: event("completed"),
    onErrorOccurred: event("error")
  });
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  listeners.get("before")(requestDetails(url, "request-late-conflict"));
  listeners.get("response")(requestDetails(
    url,
    "request-late-conflict",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));

  const observation = observer.finishWhenReady(
    attemptId,
    "open",
    100,
    CONTEXT_A
  );
  setTimeout(() => {
    listeners.get("completed")(requestDetails(
      url,
      "request-late-conflict",
      CONTEXT_A,
      { ip: "203.0.113.8" }
    ));
  }, 10);

  assert.deepEqual(await observation, {
    observedAddressClass: "non_private",
    socketOutcome: "open"
  });
});

test("keeps an earlier private address when the terminal event has no IP", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-empty-terminal"));
  observer.observeResponse(requestDetails(
    url,
    "request-empty-terminal",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));

  const observation = observer.finishWhenReady(
    attemptId,
    "open",
    100,
    CONTEXT_A
  );
  setTimeout(() => {
    observer.observeTerminal(requestDetails(
      url,
      "request-empty-terminal"
    ));
  }, 10);

  assert.deepEqual(await observation, {
    observedAddressClass: "private_ipv4",
    socketOutcome: "open"
  });
});

test("fails closed when no terminal event arrives before the bound", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-no-terminal"));
  observer.observeResponse(requestDetails(
    url,
    "request-no-terminal",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));

  assert.deepEqual(
    await observer.finishWhenReady(attemptId, "open", 20, CONTEXT_A),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
});

test("an unrelated document cannot claim a pending same-URL attempt", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);

  observer.observeBefore(requestDetails(
    url,
    "request-unrelated",
    CONTEXT_B
  ));
  observer.observeResponse(requestDetails(
    url,
    "request-unrelated",
    CONTEXT_B,
    { ip: "192.168.1.8" }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-unrelated",
    CONTEXT_B,
    { ip: "192.168.1.8" }
  ));

  observer.observeBefore(requestDetails(url, "request-actual"));
  observer.observeResponse(requestDetails(
    url,
    "request-actual",
    CONTEXT_A,
    { ip: "203.0.113.8" }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-actual",
    CONTEXT_A,
    { ip: "203.0.113.8" }
  ));

  const observation = await observer.finishWhenReady(
    attemptId,
    "open",
    100,
    CONTEXT_A
  );
  assert.equal(observation.observedAddressClass, "non_private");
  assert.equal(describeDirectTransportFailure({
    host: "harmony-web-companion.local",
    ...observation
  }).allowAuthentication, false);
});

test("same-context concurrent attempts are ambiguous and fail closed", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const firstAttempt = observer.begin(url, CONTEXT_A);
  const secondAttempt = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-ambiguous"));
  observer.observeTerminal(requestDetails(
    url,
    "request-ambiguous",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));

  assert.deepEqual(
    await observer.finishWhenReady(firstAttempt, "open", 0, CONTEXT_A),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
  assert.deepEqual(
    await observer.finishWhenReady(secondAttempt, "open", 0, CONTEXT_A),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
});

test("Edge-style missing documentId uses a matching initiator", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore({
    url,
    requestId: "request-missing-context",
    initiator: CONTEXT_NO_DOCUMENT.initiator
  });
  observer.observeTerminal({
    url,
    requestId: "request-missing-context",
    initiator: CONTEXT_NO_DOCUMENT.initiator,
    ip: "192.168.1.8"
  });
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "private_ipv4",
      socketOutcome: "open"
    }
  );
});

test("missing both webRequest initiator and matching document fails closed", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore({
    url,
    requestId: "request-without-usable-context"
  });
  observer.observeTerminal({
    url,
    requestId: "request-without-usable-context",
    ip: "192.168.1.8"
  });
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
});

test("another extension initiator cannot claim an Edge-style attempt", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore({
    url,
    requestId: "request-other-extension",
    initiator: "chrome-extension://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  });
  observer.observeTerminal({
    url,
    requestId: "request-other-extension",
    initiator: "chrome-extension://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    ip: "192.168.1.8"
  });
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
});

test("a different extension document cannot consume the finish result", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-finish-context"));
  observer.observeTerminal(requestDetails(
    url,
    "request-finish-context",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));

  assert.deepEqual(
    await observer.finishWhenReady(attemptId, "open", 0, CONTEXT_B),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
  assert.deepEqual(
    await observer.finishWhenReady(attemptId, "open", 0, CONTEXT_A),
    {
      observedAddressClass: "private_ipv4",
      socketOutcome: "open"
    }
  );
});

test("a terminal event with mismatched context invalidates the attempt", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-terminal-context"));
  observer.observeResponse(requestDetails(
    url,
    "request-terminal-context",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-terminal-context",
    CONTEXT_B
  ));

  assert.deepEqual(
    await observer.finishWhenReady(attemptId, "open", 100, CONTEXT_A),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  );
});

test("a terminal event without any address remains unresolved", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-no-address"));
  observer.observeTerminal(requestDetails(url, "request-no-address"));

  assert.deepEqual(
    await observer.finishWhenReady(attemptId, "error", 100, CONTEXT_A),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "error"
    }
  );
});

test("expired observation remains unresolved", () => {
  let now = 1_000;
  const observer = new DirectRequestObserver({ now: () => now });
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_A);
  now += 20_000;
  assert.deepEqual(observer.finish(attemptId, "error", CONTEXT_A), {
    observedAddressClass: "unresolved",
    socketOutcome: "error"
  });
});

test("registers read-only WebSocket handshake observers only", () => {
  const registrations = [];
  const event = (name) => ({
    addListener(callback, filter) {
      registrations.push({ name, callback, filter });
    }
  });
  installDirectRequestObserver(new DirectRequestObserver(), {
    onBeforeRequest: event("before"),
    onResponseStarted: event("response"),
    onCompleted: event("completed"),
    onErrorOccurred: event("error")
  });
  assert.deepEqual(registrations.map(({ name }) => name), [
    "before",
    "response",
    "completed",
    "error"
  ]);
  for (const registration of registrations) {
    assert.deepEqual(registration.filter, {
      urls: ["ws://*/*"],
      types: ["websocket"]
    });
  }
});
