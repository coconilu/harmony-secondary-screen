import { validateMediaContract } from "./video-contract.js";

export const DIRECT_PROTOCOL = 6;
export const DIRECT_PORT = 44000;
export const DEFAULT_RECEIVER_HOST = "harmony-web-companion.local";
export const DIRECT_VIDEO_MAGIC = 0x48574336;
export const DIRECT_VIDEO_HEADER_SIZE = 32;
export const DIRECT_VIDEO_MAX_PAYLOAD_BYTES = 8 * 1024 * 1024;
export const DIRECT_MAX_BUFFERED_BYTES = 4 * 1024 * 1024;
export const PAIRING_TTL_MS = 60_000;
export const DIRECT_PROOF_NONCE_BYTES = 32;

const PRIVATE_IPV4_ERROR = "v0.1 只允许可信局域网 IPv4 地址";

export function normalizeReceiverHost(value) {
  const host = String(value ?? "").trim().toLowerCase();
  if (host === DEFAULT_RECEIVER_HOST) {
    return host;
  }
  const octets = host.split(".");
  if (
    octets.length !== 4 ||
    octets.some((part) => !/^\d{1,3}$/.test(part)) ||
    octets.some((part) => Number(part) > 255)
  ) {
    throw new Error("请输入有效的平板私网 IPv4，或使用默认 .local 地址");
  }
  const [a, b] = octets.map(Number);
  const privateAddress =
    a === 10 ||
    (a === 172 && b >= 16 && b <= 31) ||
    (a === 192 && b === 168) ||
    (a === 169 && b === 254);
  if (!privateAddress) {
    throw new Error(PRIVATE_IPV4_ERROR);
  }
  return octets.map(Number).join(".");
}

export function createDirectWebSocketUrl(host) {
  return `ws://${normalizeReceiverHost(host)}:${DIRECT_PORT}/direct`;
}

export function createPairingAuthorization(now = Date.now(), randomBytes = crypto.getRandomValues.bind(crypto)) {
  const sessionId = randomHex(16, randomBytes);
  const token = randomHex(32, randomBytes);
  const expiresAt = now + PAIRING_TTL_MS;
  const shortCode = shortCodeFromToken(token);
  const payload = JSON.stringify({
    v: DIRECT_PROTOCOL,
    sid: sessionId,
    token,
    exp: expiresAt
  });
  return { sessionId, token, expiresAt, shortCode, payload };
}

export function parsePairingAuthorization(payload, now = Date.now()) {
  let value;
  try {
    value = JSON.parse(String(payload ?? ""));
  } catch {
    throw new Error("二维码或短码授权内容无效");
  }
  if (
    value?.v !== DIRECT_PROTOCOL ||
    !/^[0-9a-f]{32}$/.test(value.sid ?? "") ||
    !/^[0-9a-f]{64}$/.test(value.token ?? "") ||
    !Number.isSafeInteger(value.exp) ||
    value.exp <= now ||
    value.exp > now + PAIRING_TTL_MS + 5_000
  ) {
    throw new Error("二维码授权无效或已经过期");
  }
  return {
    sessionId: value.sid,
    token: value.token,
    expiresAt: value.exp,
    shortCode: shortCodeFromToken(value.token)
  };
}

export function shortCodeFromToken(token) {
  if (!/^[0-9a-f]{64}$/.test(token ?? "")) {
    throw new Error("配对令牌格式无效");
  }
  const value = Number.parseInt(token.slice(0, 12), 16) % 1_000_000;
  return String(value).padStart(6, "0");
}

export function createProofNonce(
  randomBytes = crypto.getRandomValues.bind(crypto)
) {
  return randomHex(DIRECT_PROOF_NONCE_BYTES, randomBytes);
}

export function createPairProofMessage({
  proofMode,
  sessionId,
  senderId,
  nonce,
  deviceId
}) {
  if (
    proofMode !== "qr" ||
    !/^[0-9a-f]{32}$/.test(sessionId ?? "") ||
    !/^[0-9a-f-]{16,64}$/.test(senderId ?? "") ||
    !isProofNonce(nonce) ||
    !/^[0-9a-f]{32}$/.test(deviceId ?? "")
  ) {
    throw new Error("配对挑战字段无效");
  }
  return [
    "HWC6-PAIR-PROOF",
    proofMode,
    sessionId,
    senderId,
    nonce,
    deviceId
  ].join("\n");
}

export function createAuthProofMessage({
  senderId,
  deviceId,
  sourceEpoch,
  nonce,
  codec = "video/avc",
  avcFormat = "annexb",
  width,
  height,
  maxFps
}) {
  let mediaContract;
  try {
    mediaContract = validateMediaContract({ width, height, maxFps });
  } catch {
    throw new Error("鉴权挑战字段无效");
  }
  if (
    !/^[0-9a-f-]{16,64}$/.test(senderId ?? "") ||
    !/^[0-9a-f]{32}$/.test(deviceId ?? "") ||
    !Number.isInteger(sourceEpoch) ||
    sourceEpoch <= 0 ||
    sourceEpoch > 0xffffffff ||
    !isProofNonce(nonce) ||
    codec !== "video/avc" ||
    avcFormat !== "annexb"
  ) {
    throw new Error("鉴权挑战字段无效");
  }
  return [
    "HWC6-AUTH-PROOF",
    senderId,
    deviceId,
    String(sourceEpoch),
    nonce,
    codec,
    avcFormat,
    String(mediaContract.width),
    String(mediaContract.height),
    String(mediaContract.maxFps)
  ].join("\n");
}

export async function computePairProof({
  token,
  proofMode,
  sessionId,
  senderId,
  nonce,
  deviceId
}, subtle = crypto.subtle) {
  const keyBytes = hexToBytes(token, 32, "配对令牌格式无效");
  return hmacSha256Hex(
    keyBytes,
    createPairProofMessage({
      proofMode,
      sessionId,
      senderId,
      nonce,
      deviceId
    }),
    subtle
  );
}

export async function computeAuthProof({
  credential,
  senderId,
  deviceId,
  sourceEpoch,
  nonce,
  codec = "video/avc",
  avcFormat = "annexb",
  width,
  height,
  maxFps
}, subtle = crypto.subtle) {
  return hmacSha256Hex(
    hexToBytes(credential, 32, "长期凭据格式无效"),
    createAuthProofMessage({
      senderId,
      deviceId,
      sourceEpoch,
      nonce,
      codec,
      avcFormat,
      width,
      height,
      maxFps
    }),
    subtle
  );
}

export function constantTimeEqualProof(left, right) {
  const first = String(left ?? "");
  const second = String(right ?? "");
  let difference = first.length ^ second.length;
  for (let index = 0; index < 64; index += 1) {
    difference |= (first.charCodeAt(index) || 0) ^
      (second.charCodeAt(index) || 0);
  }
  return difference === 0 && /^[0-9a-f]{64}$/.test(first);
}

export function isProofNonce(value) {
  return new RegExp(
    `^[0-9a-f]{${DIRECT_PROOF_NONCE_BYTES * 2}}$`
  ).test(value ?? "");
}

export function createDirectVideoMessage(chunk, sourceEpoch, sequence) {
  if (
    !chunk ||
    typeof chunk.copyTo !== "function" ||
    !Number.isSafeInteger(chunk.byteLength) ||
    chunk.byteLength < 0 ||
    chunk.byteLength > DIRECT_VIDEO_MAX_PAYLOAD_BYTES
  ) {
    throw new Error("H.264 编码块长度无效");
  }
  for (const [value, label] of [
    [sourceEpoch, "来源 epoch"],
    [sequence, "帧序号"]
  ]) {
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
      throw new Error(`${label}无效`);
    }
  }
  if (!Number.isSafeInteger(chunk.timestamp) || chunk.timestamp < 0) {
    throw new Error("H.264 编码块时间戳无效");
  }

  const message = new ArrayBuffer(
    DIRECT_VIDEO_HEADER_SIZE + chunk.byteLength
  );
  const view = new DataView(message);
  view.setUint32(0, DIRECT_VIDEO_MAGIC, false);
  view.setUint8(4, DIRECT_PROTOCOL);
  view.setUint8(5, chunk.type === "key" ? 1 : 0);
  view.setUint16(6, DIRECT_VIDEO_HEADER_SIZE, false);
  view.setUint32(8, sourceEpoch, false);
  view.setUint32(12, sequence, false);
  view.setUint32(16, chunk.byteLength, false);
  view.setUint32(20, 0, false);
  view.setBigUint64(24, BigInt(chunk.timestamp), false);
  chunk.copyTo(new Uint8Array(message, DIRECT_VIDEO_HEADER_SIZE));
  return message;
}

export function isLatestEpoch(candidate, latest) {
  return Number.isInteger(candidate) && candidate >= latest;
}

export function nextSourceEpoch(current) {
  if (!Number.isInteger(current) || current < 0 || current >= 0xffffffff) {
    return 1;
  }
  return current + 1;
}

export function validateTrustedDevice(value) {
  if (
    !value ||
    !/^[0-9a-f-]{16,64}$/.test(value.senderId ?? "") ||
    !/^[0-9a-f]{32}$/.test(value.deviceId ?? "") ||
    !/^[0-9a-f]{64}$/.test(value.credential ?? "")
  ) {
    throw new Error("已保存的平板凭据无效，请重新配对");
  }
  return {
    senderId: value.senderId,
    deviceId: value.deviceId,
    credential: value.credential,
    host: normalizeReceiverHost(value.host ?? DEFAULT_RECEIVER_HOST),
    pairedAt: Number.isSafeInteger(value.pairedAt) ? value.pairedAt : 0
  };
}

function randomHex(byteCount, randomBytes) {
  const bytes = new Uint8Array(byteCount);
  randomBytes(bytes);
  return [...bytes].map((value) => value.toString(16).padStart(2, "0")).join("");
}

function hexToBytes(value, byteCount, message) {
  const text = String(value ?? "");
  if (!new RegExp(`^[0-9a-f]{${byteCount * 2}}$`).test(text)) {
    throw new Error(message);
  }
  return Uint8Array.from(
    { length: byteCount },
    (_, index) => Number.parseInt(text.slice(index * 2, index * 2 + 2), 16)
  );
}

async function hmacSha256Hex(keyBytes, message, subtle) {
  if (
    !subtle ||
    typeof subtle.importKey !== "function" ||
    typeof subtle.sign !== "function"
  ) {
    throw new Error("当前 Edge 不支持挑战证明所需的 Web Crypto");
  }
  const key = await subtle.importKey(
    "raw",
    keyBytes,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"]
  );
  const signature = await subtle.sign(
    "HMAC",
    key,
    new TextEncoder().encode(message)
  );
  return [...new Uint8Array(signature)]
    .map((value) => value.toString(16).padStart(2, "0"))
    .join("");
}
