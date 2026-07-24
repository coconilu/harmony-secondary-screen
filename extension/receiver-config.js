export function validateReceiverConfig(receiver) {
  const address = String(receiver?.address ?? "").trim();
  const pairingCode = String(receiver?.pairingCode ?? "").trim();
  const octets = address.split(".");
  if (
    octets.length !== 4 ||
    octets.some((part) => !/^\d{1,3}$/.test(part)) ||
    octets.some((part) => Number(part) > 255)
  ) {
    throw new Error("请输入有效的平板 IPv4 地址");
  }
  const values = octets.map(Number);
  const trustedLan =
    values[0] === 10 ||
    (values[0] === 172 && values[1] >= 16 && values[1] <= 31) ||
    (values[0] === 192 && values[1] === 168) ||
    (values[0] === 169 && values[1] === 254);
  if (!trustedLan) {
    throw new Error("v0.1 只允许可信局域网 IPv4 地址");
  }
  if (!/^\d{6}$/.test(pairingCode)) {
    throw new Error("请输入平板显示的六位一次性配对码");
  }
  return {
    address: values.join("."),
    pairingCode
  };
}
