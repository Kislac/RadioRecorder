const form = document.querySelector("#settingsForm"),
  minutes = document.querySelector("#screenOffMinutes"),
  wifiMinutes = document.querySelector("#wifiAutoOffMinutes"),
  save = document.querySelector("#save"),
  wakeDisplay = document.querySelector("#wakeDisplay"),
  result = document.querySelector("#result");
const wifiStatus = document.querySelector("#wifiStatus"),
  scanButton = document.querySelector("#scanNetworks"),
  forgetButton = document.querySelector("#forgetNetwork"),
  networkList = document.querySelector("#networks"),
  connectForm = document.querySelector("#connectForm"),
  ssidInput = document.querySelector("#wifiSsid"),
  passwordInput = document.querySelector("#wifiPassword"),
  keepApInput = document.querySelector("#keepApHost");
async function loadSettings() {
  try {
    const response = await fetch("/api/settings", { cache: "no-store" });
    if (!response.ok) throw Error();
    const settings = await response.json();
    minutes.value = settings.screenOffMinutes;
    wifiMinutes.value = settings.wifiAutoOffMinutes;
    save.disabled = false;
    keepApInput.checked = !!settings.wifi.keepApHost;
    forgetButton.disabled = !settings.wifi.hasSavedNetwork;
    wifiStatus.textContent = wifiSummary(settings.wifi);
    result.textContent = "Beállítások betöltve.";
  } catch {
    result.textContent = "A beállítások betöltése sikertelen.";
  }
}
function wifiSummary(status) {
  return (
    status.mode +
    " · " +
    status.ssid +
    " · " +
    status.ip +
    (status.savedSsid ? " · mentve: " + status.savedSsid : "")
  );
}
form.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!form.reportValidity()) return;
  save.disabled = true;
  result.textContent = "Mentés...";
  try {
    const params = new URLSearchParams({
      screenOffMinutes: minutes.value,
      wifiAutoOffMinutes: wifiMinutes.value,
    });
    const response = await fetch("/api/settings?" + params, { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    const settings = await response.json();
    result.textContent =
      "Elmentve · kijelző " +
      settings.screenOffMinutes +
      " min · Wi-Fi " +
      settings.wifiAutoOffMinutes +
      " min";
  } catch (error) {
    result.textContent = error.message || "A mentés sikertelen.";
  } finally {
    save.disabled = false;
  }
});
wakeDisplay.addEventListener("click", async () => {
  wakeDisplay.disabled = true;
  try {
    const response = await fetch("/api/display/wake", { method: "POST" });
    if (!response.ok) throw Error();
    result.textContent = "A kijelző felébredt.";
  } catch {
    result.textContent = "A kijelző ébresztése sikertelen.";
  } finally {
    wakeDisplay.disabled = false;
  }
});
async function readScan() {
  const response = await fetch("/api/wifi/scan", { cache: "no-store" });
  if (!response.ok) throw Error();
  return response.json();
}
function showNetworks(data) {
  networkList.replaceChildren();
  if (data.state === "scanning") {
    wifiStatus.textContent = "Hálózatok keresése...";
    return;
  }
  if (data.state === "failed") {
    wifiStatus.textContent = "A keresés sikertelen.";
    return;
  }
  for (const network of data.networks || []) {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent =
      network.ssid +
      " · " +
      network.rssi +
      " dBm" +
      (network.open ? " · nyílt" : "");
    button.addEventListener("click", () => {
      ssidInput.value = network.ssid;
      passwordInput.focus();
    });
    networkList.append(button);
  }
  if (!data.networks?.length) wifiStatus.textContent = "Nem található hálózat.";
}
scanButton.addEventListener("click", async () => {
  scanButton.disabled = true;
  networkList.replaceChildren();
  try {
    const start = await fetch("/api/wifi/scan", { method: "POST" });
    if (!start.ok) throw Error(await start.text());
    for (let attempt = 0; attempt < 30; attempt++) {
      const data = await readScan();
      showNetworks(data);
      if (data.state !== "scanning") break;
      await new Promise((resolve) => setTimeout(resolve, 500));
    }
  } catch (error) {
    wifiStatus.textContent = error.message || "A keresés sikertelen.";
  } finally {
    scanButton.disabled = false;
  }
});
connectForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!connectForm.reportValidity()) return;
  document.querySelector("#connectWifi").disabled = true;
  wifiStatus.textContent = "Csatlakozás...";
  try {
    const body = new URLSearchParams({
      ssid: ssidInput.value,
      password: passwordInput.value,
      keepApHost: keepApInput.checked ? "1" : "0",
    });
    const response = await fetch("/api/wifi/connect", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body,
    });
    if (!response.ok) throw Error(await response.text());
    passwordInput.value = "";
    wifiStatus.textContent = "Csatlakozási kísérlet elindítva...";
    for (let attempt = 0; attempt < 30; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 500));
      const statusResponse = await fetch("/api/wifi/status", {
        cache: "no-store",
      });
      if (!statusResponse.ok) continue;
      const status = await statusResponse.json();
      wifiStatus.textContent = wifiSummary(status);
      if (!status.connecting) break;
    }
  } catch (error) {
    wifiStatus.textContent = error.message || "A csatlakozás nem indult el.";
  } finally {
    document.querySelector("#connectWifi").disabled = false;
    loadSettings();
  }
});
forgetButton.addEventListener("click", async () => {
  forgetButton.disabled = true;
  try {
    const response = await fetch("/api/wifi/forget", { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    wifiStatus.textContent = "Hálózat elfelejtve; AP mód aktív.";
    keepApInput.checked = false;
    passwordInput.value = "";
  } catch (error) {
    wifiStatus.textContent =
      error.message || "A hálózat elfelejtése sikertelen.";
  } finally {
    loadSettings();
  }
});
loadSettings();
