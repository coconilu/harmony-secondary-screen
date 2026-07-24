const form = document.querySelector("#receiver-form");
const address = document.querySelector("#receiver-address");
const pairingCode = document.querySelector("#pairing-code");
const startButton = document.querySelector("#start-button");
const errorMessage = document.querySelector("#error-message");

form.addEventListener("submit", (event) => {
  event.preventDefault();
  void startCapture();
});

async function startCapture() {
  errorMessage.hidden = true;
  startButton.disabled = true;
  startButton.textContent = "正在连接平板…";
  try {
    const response = await chrome.runtime.sendMessage({
      target: "service-worker",
      type: "START_CAPTURE",
      receiver: {
        address: address.value,
        pairingCode: pairingCode.value
      }
    });
    pairingCode.value = "";
    if (!response?.ok) {
      throw new Error(response?.error || "启动失败");
    }
    window.close();
  } catch (error) {
    pairingCode.value = "";
    errorMessage.textContent =
      error instanceof Error ? error.message : String(error);
    errorMessage.hidden = false;
    startButton.disabled = false;
    startButton.textContent = "发送当前标签页";
  }
}
