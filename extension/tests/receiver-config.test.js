import assert from "node:assert/strict";
import test from "node:test";

import { validateReceiverConfig } from "../receiver-config.js";

test("accepts and normalizes trusted LAN receiver configuration", () => {
  assert.deepEqual(
    validateReceiverConfig({
      address: " 192.168.003.112 ",
      pairingCode: "406313"
    }),
    {
      address: "192.168.3.112",
      pairingCode: "406313"
    }
  );
});

test("rejects public, loopback, wildcard and malformed addresses", () => {
  for (const address of [
    "8.8.8.8",
    "127.0.0.1",
    "0.0.0.0",
    "192.168.3.999",
    "not-an-ip"
  ]) {
    assert.throws(
      () => validateReceiverConfig({ address, pairingCode: "123456" })
    );
  }
});

test("rejects malformed pairing codes", () => {
  assert.throws(
    () => validateReceiverConfig({
      address: "192.168.3.112",
      pairingCode: "12345"
    }),
    /六位/
  );
});
