import {
  DEFAULT_RECEIVER_HOST,
  LEGACY_DEFAULT_RECEIVER_HOST,
  normalizeReceiverHost
} from "./direct-protocol.js";

const LEGACY_DEFAULT_RECEIVER_ORIGIN =
  `http://${LEGACY_DEFAULT_RECEIVER_HOST}/*`;

function permissionApiOrDefault(permissions) {
  return permissions ?? chrome.permissions;
}

export function manualHostOrigin(host) {
  const normalized = normalizeReceiverHost(host);
  return normalized === DEFAULT_RECEIVER_HOST
    ? null
    : `http://${normalized}/*`;
}

export async function acquireHostPermission(host, permissions) {
  const origin = manualHostOrigin(host);
  if (!origin) {
    return { origin: null, added: false };
  }
  const api = permissionApiOrDefault(permissions);
  if (await api.contains({ origins: [origin] })) {
    return { origin, added: false };
  }
  if (!await api.request({ origins: [origin] })) {
    throw new Error("需要允许访问你刚输入的平板私网地址");
  }
  return { origin, added: true };
}

export async function hasHostPermission(host, permissions) {
  const origin = manualHostOrigin(host);
  if (!origin) return true;
  return permissionApiOrDefault(permissions).contains({ origins: [origin] });
}

export async function revokeHostPermission(origin, permissions) {
  if (!origin) return;
  const removed = await permissionApiOrDefault(permissions).remove({
    origins: [origin]
  });
  if (!removed) {
    throw new Error(`无法撤销未使用的地址权限：${origin}`);
  }
}

export async function withHostPermission(host, operation, permissions) {
  const lease = await acquireHostPermission(host, permissions);
  try {
    return await operation(lease);
  } catch (error) {
    if (lease.added) {
      try {
        await revokeHostPermission(lease.origin, permissions);
      } catch (rollbackError) {
        throw new AggregateError(
          [error, rollbackError],
          "操作失败，且临时地址权限未能撤销"
        );
      }
    }
    throw error;
  }
}

function isManualPrivateIpv4Origin(origin) {
  const match = /^http:\/\/(\d{1,3}(?:\.\d{1,3}){3})\/\*$/.exec(origin);
  if (!match) return false;
  try {
    return normalizeReceiverHost(match[1]) === match[1];
  } catch {
    return false;
  }
}

export async function cleanupUnusedManualHostPermissions(
  usedHosts,
  permissions
) {
  const api = permissionApiOrDefault(permissions);
  const usedOrigins = new Set(
    usedHosts.map(manualHostOrigin).filter(Boolean)
  );
  const granted = await api.getAll();
  const unused = (granted.origins ?? []).filter(
    (origin) =>
      origin === LEGACY_DEFAULT_RECEIVER_ORIGIN ||
      (isManualPrivateIpv4Origin(origin) && !usedOrigins.has(origin))
  );
  for (const origin of unused) {
    await revokeHostPermission(origin, api);
  }
  return unused;
}
