import test from "node:test";
import assert from "node:assert/strict";

import {
  DirectRequestObserver,
  handleDirectObservationMessage
} from "../direct-network-diagnostics.js";

const EXTENSION_ID = "abcdefghijklmnopabcdefghijklmnop";
const EXTENSION_ORIGIN = `chrome-extension://${EXTENSION_ID}`;
const DIRECT_URL = "ws://harmony-web-companion.local:44000/direct";
const runtimeApi = Object.freeze({
  id: EXTENSION_ID,
  getURL: (path) => `${EXTENSION_ORIGIN}/${path}`
});

function dispatch(observer, message, sender) {
  return handleDirectObservationMessage(message, sender, {
    observer,
    runtimeApi
  });
}

function edgeSender(page = "setup.html") {
  return {
    id: EXTENSION_ID,
    url: runtimeApi.getURL(page)
  };
}

function edgeRequest(requestId, extra = {}) {
  return {
    requestId,
    url: DIRECT_URL,
    type: "websocket",
    tabId: -1,
    frameId: 0,
    parentFrameId: -1,
    ...extra
  };
}

test("service worker accepts an Edge sender without optional origin or documentId", async () => {
  const observer = new DirectRequestObserver();
  const begin = dispatch(observer, {
    type: "BEGIN_DIRECT_OBSERVATION",
    url: DIRECT_URL
  }, edgeSender());
  assert.equal(begin.handled, true);
  assert.equal(begin.keepChannelOpen, false);
  assert.equal(begin.response.ok, true);

  observer.observeBefore(edgeRequest("edge-request"));
  observer.observeTerminal(edgeRequest("edge-request", {
    ip: "192.168.1.8"
  }));

  const finish = dispatch(observer, {
    type: "FINISH_DIRECT_OBSERVATION",
    attemptId: begin.response.attemptId,
    socketOutcome: "open",
    waitMs: 0
  }, edgeSender());
  assert.equal(finish.handled, true);
  assert.equal(finish.keepChannelOpen, true);
  assert.deepEqual(await finish.responsePromise, {
    ok: true,
    observation: {
      observedAddressClass: "private_ipv4",
      socketOutcome: "open"
    }
  });
});

test("service worker rejects external, other-extension and unapproved pages", () => {
  for (const sender of [
    {
      id: EXTENSION_ID,
      url: "https://example.test/setup.html"
    },
    {
      id: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      url: runtimeApi.getURL("setup.html")
    },
    {
      id: EXTENSION_ID,
      url: runtimeApi.getURL("monitor.html")
    }
  ]) {
    const result = dispatch(new DirectRequestObserver(), {
      type: "BEGIN_DIRECT_OBSERVATION",
      url: DIRECT_URL
    }, sender);
    assert.equal(result.handled, true);
    assert.equal(result.keepChannelOpen, false);
    assert.equal(result.response.ok, false);
  }
});

test("service worker keeps no-document attempts fail-closed when ambiguous", async () => {
  const observer = new DirectRequestObserver();
  const first = dispatch(observer, {
    type: "BEGIN_DIRECT_OBSERVATION",
    url: DIRECT_URL
  }, edgeSender());
  const second = dispatch(observer, {
    type: "BEGIN_DIRECT_OBSERVATION",
    url: DIRECT_URL
  }, edgeSender());
  observer.observeBefore(edgeRequest("ambiguous-request"));
  observer.observeTerminal(edgeRequest("ambiguous-request", {
    ip: "192.168.1.8"
  }));

  for (const attemptId of [
    first.response.attemptId,
    second.response.attemptId
  ]) {
    const finish = dispatch(observer, {
      type: "FINISH_DIRECT_OBSERVATION",
      attemptId,
      socketOutcome: "open",
      waitMs: 0
    }, edgeSender());
    assert.deepEqual(await finish.responsePromise, {
      ok: true,
      observation: {
        observedAddressClass: "unresolved",
        socketOutcome: "open"
      }
    });
  }
});

test("service worker rejects a wrong FINISH page without consuming the attempt", async () => {
  const observer = new DirectRequestObserver();
  const begin = dispatch(observer, {
    type: "BEGIN_DIRECT_OBSERVATION",
    url: DIRECT_URL
  }, edgeSender());
  observer.observeBefore(edgeRequest("finish-request"));
  observer.observeTerminal(edgeRequest("finish-request", {
    ip: "192.168.1.8"
  }));

  const wrongFinish = dispatch(observer, {
    type: "FINISH_DIRECT_OBSERVATION",
    attemptId: begin.response.attemptId,
    socketOutcome: "open",
    waitMs: 0
  }, edgeSender("offscreen.html"));
  assert.deepEqual(await wrongFinish.responsePromise, {
    ok: true,
    observation: {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  });

  const correctFinish = dispatch(observer, {
    type: "FINISH_DIRECT_OBSERVATION",
    attemptId: begin.response.attemptId,
    socketOutcome: "open",
    waitMs: 0
  }, edgeSender());
  assert.deepEqual(await correctFinish.responsePromise, {
    ok: true,
    observation: {
      observedAddressClass: "private_ipv4",
      socketOutcome: "open"
    }
  });
});

test("service worker returns unresolved for an unknown FINISH capability", async () => {
  const observer = new DirectRequestObserver();
  const finish = dispatch(observer, {
    type: "FINISH_DIRECT_OBSERVATION",
    attemptId: "not-an-issued-attempt",
    socketOutcome: "open",
    waitMs: 0
  }, edgeSender());
  assert.deepEqual(await finish.responsePromise, {
    ok: true,
    observation: {
      observedAddressClass: "unresolved",
      socketOutcome: "open"
    }
  });
});
