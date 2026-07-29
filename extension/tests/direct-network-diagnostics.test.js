import test from "node:test";
import assert from "node:assert/strict";

import {
  classifyObservedAddress,
  describeDirectTransportFailure,
  DirectRequestObserver,
  installDirectRequestObserver
} from "../direct-network-diagnostics.js";

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

test("correlates one WebSocket request without persisting its raw address", async () => {
  let now = 1_000;
  const observer = new DirectRequestObserver({ now: () => now });
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url);
  observer.observeBefore({ url, requestId: "request-1" });
  observer.observeAddress({ requestId: "request-1", ip: "192.168.1.8" });
  assert.deepEqual(await observer.finishWhenReady(attemptId, "open"), {
    observedAddressClass: "private_ipv4",
    socketOutcome: "open"
  });

  const expired = observer.begin(url);
  now += 20_000;
  assert.deepEqual(observer.finish(expired, "error"), {
    observedAddressClass: "unresolved",
    socketOutcome: "error"
  });
});

test("waits for the completed handshake when response-started omits its IP", async () => {
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
  const attemptId = observer.begin(url);
  listeners.get("before")({ url, requestId: "request-late-ip" });
  listeners.get("response")({ requestId: "request-late-ip" });

  const observation = observer.finishWhenReady(attemptId, "open", 100);
  setTimeout(() => {
    listeners.get("completed")({
      requestId: "request-late-ip",
      ip: "192.168.1.8"
    });
  }, 10);

  assert.deepEqual(await observation, {
    observedAddressClass: "private_ipv4",
    socketOutcome: "open"
  });
});

test("an empty later event cannot erase an observed private address", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url);
  observer.observeBefore({ url, requestId: "request-empty-late" });
  observer.observeAddress({
    requestId: "request-empty-late",
    ip: "192.168.1.8"
  });
  observer.observeAddress({ requestId: "request-empty-late" });

  assert.deepEqual(await observer.finishWhenReady(attemptId, "open"), {
    observedAddressClass: "private_ipv4",
    socketOutcome: "open"
  });
});

test("conflicting address classes for one request fail closed", async () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const attemptId = observer.begin(url);
  observer.observeBefore({ url, requestId: "request-conflict" });
  observer.observeAddress({
    requestId: "request-conflict",
    ip: "192.168.1.8"
  });
  observer.observeAddress({
    requestId: "request-conflict",
    ip: "203.0.113.8"
  });

  const observation = await observer.finishWhenReady(attemptId, "open");
  assert.equal(observation.observedAddressClass, "non_private");
  assert.equal(describeDirectTransportFailure({
    host: "harmony-web-companion.local",
    ...observation
  }).allowAuthentication, false);
});

test("correlates concurrent same-URL attempts in request start order", () => {
  const observer = new DirectRequestObserver();
  const url = "ws://harmony-web-companion.local:44000/direct";
  const firstAttempt = observer.begin(url);
  const secondAttempt = observer.begin(url);
  observer.observeBefore({ url, requestId: "request-first" });
  observer.observeBefore({ url, requestId: "request-second" });
  observer.observeAddress({
    requestId: "request-first",
    ip: "192.168.1.8"
  });
  observer.observeAddress({
    requestId: "request-second",
    ip: "203.0.113.8"
  });

  assert.equal(
    observer.finish(firstAttempt, "open").observedAddressClass,
    "private_ipv4"
  );
  assert.equal(
    observer.finish(secondAttempt, "open").observedAddressClass,
    "non_private"
  );
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
