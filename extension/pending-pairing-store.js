import {
  DIRECT_PROTOCOL,
  DEFAULT_RECEIVER_HOST,
  LEGACY_DEFAULT_RECEIVER_HOST,
  parsePairingAuthorization
} from "./direct-protocol.js";

export const PENDING_PAIRING_STORAGE_KEY = "pendingPairing";

function normalizePendingHost(value) {
  const host = String(value ?? DEFAULT_RECEIVER_HOST).trim();
  if (!host || host.length > 255 || /[\u0000-\u001f\u007f]/.test(host)) {
    throw new Error("待配对地址无效");
  }
  if (host.toLowerCase() === LEGACY_DEFAULT_RECEIVER_HOST) {
    return DEFAULT_RECEIVER_HOST;
  }
  return host;
}

function normalizePendingPairing(value, now) {
  const candidate = value?.authorization;
  const payload = JSON.stringify({
    v: DIRECT_PROTOCOL,
    sid: candidate?.sessionId,
    token: candidate?.token,
    exp: candidate?.expiresAt
  });
  const authorization = parsePairingAuthorization(payload, now);
  if (candidate?.shortCode !== authorization.shortCode) {
    throw new Error("待配对短码无效");
  }
  return {
    host: normalizePendingHost(value.host),
    authorization: {
      ...authorization,
      payload
    }
  };
}

export async function getPendingPairing(
  storage = chrome.storage.session,
  now = Date.now()
) {
  const result = await storage.get(PENDING_PAIRING_STORAGE_KEY);
  const stored = result[PENDING_PAIRING_STORAGE_KEY];
  if (!stored) return null;
  let normalized;
  try {
    normalized = normalizePendingPairing(stored, now);
  } catch {
    await storage.remove(PENDING_PAIRING_STORAGE_KEY);
    return null;
  }
  if (stored.host !== normalized.host) {
    await storage.set({
      [PENDING_PAIRING_STORAGE_KEY]: {
        ...stored,
        host: normalized.host
      }
    });
  }
  return normalized;
}

export async function savePendingPairing(
  pending,
  storage = chrome.storage.session,
  now = Date.now()
) {
  const normalized = normalizePendingPairing(pending, now);
  const stored = {
    host: normalized.host,
    authorization: {
      sessionId: normalized.authorization.sessionId,
      token: normalized.authorization.token,
      shortCode: normalized.authorization.shortCode,
      expiresAt: normalized.authorization.expiresAt
    }
  };
  await storage.set({ [PENDING_PAIRING_STORAGE_KEY]: stored });
  return normalized;
}

export async function clearPendingPairing(
  storage = chrome.storage.session
) {
  await storage.remove(PENDING_PAIRING_STORAGE_KEY);
}
