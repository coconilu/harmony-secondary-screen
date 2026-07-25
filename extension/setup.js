import qrcode from "./vendor/qrcode.mjs";
import { pairReceiver } from "./direct-client.js";
import {
  createPairingAuthorization,
  DEFAULT_RECEIVER_HOST,
  normalizeReceiverHost
} from "./direct-protocol.js";
import {
  forgetTrustedReceiver,
  getOrCreateSenderId,
  getTrustedReceiver,
  saveTrustedReceiver
} from "./pairing-store.js";
import {
  cleanupUnusedManualHostPermissions,
  withHostPermission
} from "./host-permissions.js";

const form = document.querySelector("#pairing-form");
const pairedPanel = document.querySelector("#paired-panel");
const address = document.querySelector("#receiver-address");
const pairingQr = document.querySelector("#pairing-qr");
const pairedAddress = document.querySelector("#paired-address");
const pairedHost = document.querySelector("#paired-host");
const shortCode = document.querySelector("#short-code");
const pairButton = document.querySelector("#pair-button");
const refreshButton = document.querySelector("#refresh-button");
const startButton = document.querySelector("#start-button");
const forgetButton = document.querySelector("#forget-button");
const updateHostButton = document.querySelector("#update-host-button");
const errorMessage = document.querySelector("#error-message");
let authorization = null;

form.addEventListener("submit", (event) => {
  event.preventDefault();
  void completePairing();
});
refreshButton.addEventListener("click", refreshAuthorization);
startButton.addEventListener("click", startCapture);
forgetButton.addEventListener("click", forgetPairing);
updateHostButton.addEventListener("click", updateTrustedHost);

export const setupReady = loadPairingState();

export async function loadPairingState() {
  const trusted = await getTrustedReceiver();
  renderTrusted(trusted);
  let cleanupSucceeded = true;
  try {
    await cleanupUnusedManualHostPermissions(trusted ? [trusted.host] : []);
  } catch (error) {
    cleanupSucceeded = false;
    showError(error);
  }
  if (!trusted) {
    refreshAuthorization({ clearError: cleanupSucceeded });
  }
}

function refreshAuthorization({ clearError = true } = {}) {
  authorization = createPairingAuthorization();
  shortCode.textContent = `摄像头不可用时，在平板输入短码 ${authorization.shortCode}`;
  renderQrCode(authorization.payload);
  if (clearError) {
    errorMessage.hidden = true;
  }
}

async function completePairing() {
  errorMessage.hidden = true;
  pairButton.disabled = true;
  pairButton.textContent = "正在连接平板…";
  try {
    if (!authorization || authorization.expiresAt <= Date.now()) {
      refreshAuthorization();
      throw new Error("二维码已过期，请用平板扫描新二维码");
    }
    const host = normalizeReceiverHost(address.value);
    const senderId = await getOrCreateSenderId();
    const trusted = await withHostPermission(host, async () => {
      const paired = await pairReceiver({
        host,
        authorization,
        senderId
      });
      return saveTrustedReceiver(paired);
    });
    authorization = null;
    renderTrusted(trusted);
    await cleanupUnusedManualHostPermissions([trusted.host]);
  } catch (error) {
    showError(error);
  } finally {
    pairButton.disabled = false;
    pairButton.textContent = "已扫码，连接平板";
  }
}

async function startCapture() {
  errorMessage.hidden = true;
  startButton.disabled = true;
  startButton.textContent = "正在连接平板…";
  try {
    const response = await chrome.runtime.sendMessage({
      target: "service-worker",
      type: "START_CAPTURE"
    });
    if (!response?.ok) {
      throw new Error(response?.error || "启动失败");
    }
    window.close();
  } catch (error) {
    showError(error);
    startButton.disabled = false;
    startButton.textContent = "发送当前标签页";
  }
}

export async function forgetPairing() {
  errorMessage.hidden = true;
  let cleanupSucceeded = true;
  try {
    await forgetTrustedReceiver();
    await cleanupUnusedManualHostPermissions([]);
  } catch (error) {
    cleanupSucceeded = false;
    showError(error);
  }
  renderTrusted(null);
  refreshAuthorization({ clearError: cleanupSucceeded });
}

async function updateTrustedHost() {
  errorMessage.hidden = true;
  try {
    const trusted = await getTrustedReceiver();
    if (!trusted) {
      throw new Error("没有可更新的已配对平板");
    }
    const host = normalizeReceiverHost(pairedHost.value);
    const updated = await withHostPermission(
      host,
      () => saveTrustedReceiver({ ...trusted, host })
    );
    renderTrusted(updated);
    await cleanupUnusedManualHostPermissions([updated.host]);
  } catch (error) {
    showError(error);
  }
}

function renderTrusted(trusted) {
  const paired = Boolean(trusted);
  form.hidden = paired;
  pairedPanel.hidden = !paired;
  pairedAddress.textContent = paired
    ? `设备 ${trusted.deviceId.slice(0, 8)} · ${trusted.host}`
    : "";
  pairedHost.value = paired ? trusted.host : DEFAULT_RECEIVER_HOST;
}

function renderQrCode(payload) {
  const qr = qrcode(0, "L");
  qr.addData(payload, "Byte");
  qr.make();
  const context = pairingQr.getContext("2d");
  const count = qr.getModuleCount();
  const quiet = 4;
  const scale = Math.floor(pairingQr.width / (count + quiet * 2));
  const size = scale * (count + quiet * 2);
  const offset = Math.floor((pairingQr.width - size) / 2);
  context.fillStyle = "#fff";
  context.fillRect(0, 0, pairingQr.width, pairingQr.height);
  context.fillStyle = "#07111f";
  for (let row = 0; row < count; row += 1) {
    for (let column = 0; column < count; column += 1) {
      if (qr.isDark(row, column)) {
        context.fillRect(
          offset + (column + quiet) * scale,
          offset + (row + quiet) * scale,
          scale,
          scale
        );
      }
    }
  }
}

function showError(error) {
  errorMessage.textContent =
    error instanceof Error ? error.message : String(error);
  errorMessage.hidden = false;
}
