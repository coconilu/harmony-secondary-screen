export const LOCAL_RELAY_PROTOCOL = 1;
export const LOCAL_VIDEO_MAGIC = 0x48574c31;
export const LOCAL_VIDEO_HEADER_SIZE = 24;
export const LOCAL_VIDEO_MAX_PAYLOAD_BYTES = 8 * 1024 * 1024;

export function createLocalVideoMessage(chunk, sequence) {
  if (
    !chunk ||
    typeof chunk.copyTo !== "function" ||
    !Number.isSafeInteger(chunk.byteLength) ||
    chunk.byteLength < 0 ||
    chunk.byteLength > LOCAL_VIDEO_MAX_PAYLOAD_BYTES
  ) {
    throw new Error("H.264 编码块长度无效");
  }
  if (!Number.isInteger(sequence) || sequence < 0 || sequence > 0xffffffff) {
    throw new Error("本地 Relay 帧序号无效");
  }
  if (!Number.isSafeInteger(chunk.timestamp) || chunk.timestamp < 0) {
    throw new Error("H.264 编码块时间戳无效");
  }

  const message = new ArrayBuffer(
    LOCAL_VIDEO_HEADER_SIZE + chunk.byteLength
  );
  const view = new DataView(message);
  view.setUint32(0, LOCAL_VIDEO_MAGIC, false);
  view.setUint8(4, LOCAL_RELAY_PROTOCOL);
  view.setUint8(5, chunk.type === "key" ? 1 : 0);
  view.setUint16(6, LOCAL_VIDEO_HEADER_SIZE, false);
  view.setUint32(8, sequence, false);
  view.setUint32(12, chunk.byteLength, false);
  view.setBigUint64(16, BigInt(chunk.timestamp), false);
  chunk.copyTo(new Uint8Array(message, LOCAL_VIDEO_HEADER_SIZE));
  return message;
}
