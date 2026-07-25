import {
  DEFAULT_RECEIVER_HOST,
  nextSourceEpoch,
  validateTrustedDevice
} from "./direct-protocol.js";

export const PAIRING_STORAGE_KEY = "trustedReceiver";
export const SENDER_ID_STORAGE_KEY = "senderId";
export const SOURCE_EPOCH_STORAGE_KEY = "sourceEpoch";

export async function getOrCreateSenderId(storage = chrome.storage.local) {
  const existing = await storage.get(SENDER_ID_STORAGE_KEY);
  if (/^[0-9a-f-]{16,64}$/.test(existing[SENDER_ID_STORAGE_KEY] ?? "")) {
    return existing[SENDER_ID_STORAGE_KEY];
  }
  const senderId = crypto.randomUUID();
  await storage.set({ [SENDER_ID_STORAGE_KEY]: senderId });
  return senderId;
}

export async function getTrustedReceiver(storage = chrome.storage.local) {
  const result = await storage.get(PAIRING_STORAGE_KEY);
  if (!result[PAIRING_STORAGE_KEY]) {
    return null;
  }
  try {
    return validateTrustedDevice(result[PAIRING_STORAGE_KEY]);
  } catch {
    await storage.remove(PAIRING_STORAGE_KEY);
    return null;
  }
}

export async function saveTrustedReceiver(
  trustedReceiver,
  storage = chrome.storage.local
) {
  const normalized = validateTrustedDevice({
    ...trustedReceiver,
    host: trustedReceiver.host ?? DEFAULT_RECEIVER_HOST
  });
  await storage.set({ [PAIRING_STORAGE_KEY]: normalized });
  return normalized;
}

export async function forgetTrustedReceiver(storage = chrome.storage.local) {
  await storage.remove(PAIRING_STORAGE_KEY);
}

export async function allocateSourceEpoch(storage = chrome.storage.local) {
  const result = await storage.get(SOURCE_EPOCH_STORAGE_KEY);
  const sourceEpoch = nextSourceEpoch(result[SOURCE_EPOCH_STORAGE_KEY]);
  await storage.set({ [SOURCE_EPOCH_STORAGE_KEY]: sourceEpoch });
  return sourceEpoch;
}
