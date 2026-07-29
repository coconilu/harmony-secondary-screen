import test from "node:test";
import assert from "node:assert/strict";

import {
  beginDirectTransportObservation,
  classifyObservedAddress,
  createDirectObservationContext,
  describeDirectTransportFailure,
  DirectRequestObserver,
  finishDirectTransportObservation,
  installDirectRequestObserver,
  isDirectObservationDiagnosticCode
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
    type: "websocket",
    tabId: -1,
    frameId: 0,
    parentFrameId: -1,
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
        diagnosticCode:
          "AD1|B=0|Q=not_seen|R=0|T=none|I=0|C=0|S=open",
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

test("diagnostic codes are stable enums and never echo supplied values", () => {
  const secret = [
    "ws://harmony-web-companion.local:44000/direct",
    "request-sensitive",
    "document-sensitive",
    "chrome-extension://sensitive",
    "token-sensitive",
    "123456",
    "credential-sensitive"
  ].join(":");
  const verdict = describeDirectTransportFailure({
    host: "harmony-web-companion.local",
    socketOutcome: "open",
    observedAddressClass: "unresolved",
    diagnosticCode: `AD1|B=1|Q=${secret}`
  });
  assert.equal(verdict.message.includes("AD1"), false);
  assert.match(verdict.diagnosticCode, /^AD1\|B=0\|Q=not_seen/);
  assert.equal(verdict.message.includes(secret), false);
  assert.equal(verdict.diagnosticCode.includes(secret), false);

  const manual = describeDirectTransportFailure({
    host: "192.168.1.8",
    socketOutcome: "error",
    observedAddressClass: "unresolved",
    diagnosticCode:
      "AD1|B=1|Q=bound|R=1|T=error|I=1|C=0|S=error"
  });
  assert.equal("diagnosticCode" in manual, false);
  assert.equal(manual.message.includes("AD1"), false);

  const connected = describeDirectTransportFailure({
    host: "harmony-web-companion.local",
    socketOutcome: "open",
    observedAddressClass: "private_ipv4",
    diagnosticCode: secret
  });
  assert.equal(connected.code, "connected");
  assert.equal(connected.message, "");
  assert.equal("diagnosticCode" in connected, false);
});

test("accepts only one complete stable AD1 code", () => {
  const diagnostic =
    "AD1|B=1|Q=shape_parent|R=1|T=completed|I=1|C=0|S=open";
  assert.equal(isDirectObservationDiagnosticCode(diagnostic), true);
  for (const invalid of [
    `${diagnostic}${diagnostic}`,
    `prefix ${diagnostic}`,
    `${diagnostic} suffix`,
    "AD1|url=private",
    "AD1|B=9|Q=bound|R=1|T=completed|I=1|C=0|S=open",
    null
  ]) {
    assert.equal(isDirectObservationDiagnosticCode(invalid), false);
  }
});

test("diagnoses every initial request-shape and explicit-context rejection", () => {
  const url = "ws://harmony-web-companion.local:44000/direct";
  const cases = [
    ["request_id", { requestId: undefined }],
    ["request_id", { requestId: 42 }],
    ["shape_type", { type: undefined }],
    ["shape_tab", { tabId: 8 }],
    ["shape_frame", { frameId: -1 }],
    ["shape_parent", { parentFrameId: undefined }],
    ["initiator", {
      initiator: "chrome-extension://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    }],
    ["document", { documentId: "document-other" }]
  ];
  for (const [expected, extra] of cases) {
    const observer = new DirectRequestObserver();
    const attemptId = observer.begin(url, CONTEXT_A);
    observer.observeBefore(requestDetails(
      url,
      "request-sensitive",
      CONTEXT_A,
      extra
    ));
    const observation = observer.finish(
      attemptId,
      "open",
      CONTEXT_A
    );
    assert.equal(
      observation.diagnosticCode,
      `AD1|B=1|Q=${expected}|R=0|T=none|I=0|C=0|S=open`
    );
    assert.equal(observation.observedAddressClass, "unresolved");
    assert.equal(observer.attempts.size, 0);
  }
});

test("diagnoses response, terminal, IP, invalid context and socket outcome", () => {
  const url = "ws://harmony-web-companion.local:44000/direct";
  const observer = new DirectRequestObserver();
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeResponse(requestDetails(
    url,
    "request-sensitive",
    CONTEXT_A,
    { ip: "192.168.123.45" }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-sensitive",
    CONTEXT_A
  ), "error");

  const observation = observer.finish(
    attemptId,
    "timeout",
    CONTEXT_A
  );
  assert.deepEqual(observation, {
    observedAddressClass: "unresolved",
    socketOutcome: "timeout",
    diagnosticCode:
      "AD1|B=1|Q=not_seen|R=1|T=error|I=1|C=0|S=timeout"
  });
  assert.equal(observer.attempts.size, 0);
  for (const sensitive of [
    url,
    "request-sensitive",
    "document-a",
    EXTENSION_ORIGIN,
    "192.168.123.45"
  ]) {
    assert.equal(observation.diagnosticCode.includes(sensitive), false);
  }
});

test("conflicting terminal events diagnose invalid context and fail closed", () => {
  const url = "ws://harmony-web-companion.local:44000/direct";
  const observer = new DirectRequestObserver();
  const attemptId = observer.begin(url, CONTEXT_A);
  observer.observeBefore(requestDetails(url, "request-terminal-multiple"));
  observer.observeTerminal(requestDetails(
    url,
    "request-terminal-multiple",
    CONTEXT_A,
    { ip: "192.168.1.8" }
  ), "completed");
  observer.observeTerminal(requestDetails(
    url,
    "request-terminal-multiple",
    CONTEXT_A
  ), "error");
  assert.deepEqual(observer.finish(attemptId, "open", CONTEXT_A), {
    observedAddressClass: "unresolved",
    socketOutcome: "open",
    diagnosticCode:
      "AD1|B=1|Q=bound|R=0|T=multiple|I=1|C=1|S=open"
  });
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
    socketOutcome: "error",
    diagnosticCode:
      "AD1|B=1|Q=expired|R=0|T=none|I=0|C=0|S=error"
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
    socketOutcome: "open",
    diagnosticCode:
      "AD1|B=1|Q=bound|R=1|T=completed|I=1|C=0|S=open"
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
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=bound|R=1|T=none|I=1|C=0|S=open"
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
  assert.deepEqual(observation, {
    observedAddressClass: "unresolved",
    socketOutcome: "open",
    diagnosticCode:
      "AD1|B=1|Q=bound|R=1|T=completed|I=1|C=1|S=open"
  });
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
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=ambiguous|R=0|T=completed|I=1|C=1|S=open"
    }
  );
  assert.deepEqual(
    await observer.finishWhenReady(secondAttempt, "open", 0, CONTEXT_A),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=ambiguous|R=0|T=completed|I=1|C=1|S=open"
    }
  );
});

test("Edge-style missing documentId uses a matching initiator", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-missing-context",
    CONTEXT_NO_DOCUMENT
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-missing-context",
    CONTEXT_NO_DOCUMENT,
    { ip: "192.168.1.8" }
  ));
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

test("Edge extension-document shape binds when optional context is absent", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-without-optional-context",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined
    }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-without-optional-context",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined,
      ip: "192.168.1.8"
    }
  ));
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

test("opaque initiator uses the bounded extension-document fallback", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-opaque-initiator",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: "null"
    }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-opaque-initiator",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: "null",
      ip: "192.168.1.8"
    }
  ));
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

test("an external tab cannot claim the no-context fallback", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-external-tab",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined,
      tabId: 42
    }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-external-tab",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined,
      tabId: 42,
      ip: "192.168.1.8"
    }
  ));
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=shape_tab|R=0|T=completed|I=1|C=0|S=open"
    }
  );
});

test("an external worker cannot claim the no-context fallback", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-external-worker",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined,
      frameId: -1
    }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-external-worker",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined,
      frameId: -1,
      ip: "192.168.1.8"
    }
  ));
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=shape_frame|R=0|T=completed|I=1|C=0|S=open"
    }
  );
});

test("another extension initiator cannot claim an Edge-style attempt", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-other-extension",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: "chrome-extension://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-other-extension",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: "chrome-extension://bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      ip: "192.168.1.8"
    }
  ));
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=initiator|R=0|T=completed|I=1|C=0|S=open"
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
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=0|Q=not_seen|R=0|T=none|I=0|C=1|S=open"
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
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=bound|R=1|T=completed|I=1|C=1|S=open"
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
      socketOutcome: "error",
      diagnosticCode:
        "AD1|B=1|Q=bound|R=0|T=completed|I=0|C=0|S=error"
    }
  );
});

test("a different requestId cannot provide the private terminal result", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url, CONTEXT_NO_DOCUMENT);
  observer.observeBefore(requestDetails(
    url,
    "request-bound",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined
    }
  ));
  observer.observeTerminal(requestDetails(
    url,
    "request-not-bound",
    CONTEXT_NO_DOCUMENT,
    {
      documentId: undefined,
      initiator: undefined,
      ip: "192.168.1.8"
    }
  ));
  assert.deepEqual(
    await observer.finishWhenReady(
      attemptId,
      "open",
      0,
      CONTEXT_NO_DOCUMENT
    ),
    {
      observedAddressClass: "unresolved",
      socketOutcome: "open",
      diagnosticCode:
        "AD1|B=1|Q=bound|R=0|T=none|I=0|C=0|S=open"
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
    socketOutcome: "error",
    diagnosticCode:
      "AD1|B=1|Q=expired|R=0|T=none|I=0|C=0|S=error"
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
