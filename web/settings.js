const form = document.querySelector("#settingsForm"),
  minutes = document.querySelector("#screenOffMinutes"),
  wifiMinutes = document.querySelector("#wifiAutoOffMinutes"),
  save = document.querySelector("#save"),
  wakeDisplay = document.querySelector("#wakeDisplay"),
  result = document.querySelector("#result");
const performanceToggle = document.querySelector("#performanceEnabled"),
  performanceRefresh = document.querySelector("#refreshPerformance"),
  performanceStatus = document.querySelector("#performanceStatus"),
  performanceSnapshot = document.querySelector("#performanceSnapshot"),
  performanceTimings = document.querySelector("#performanceTimings"),
  performanceDetails = document.querySelector("#performanceDetails"),
  performanceLogToggle = document.querySelector("#performanceLogEnabled"),
  performanceLogStatus = document.querySelector("#performanceLogStatus"),
  performanceLogDownload = document.querySelector("#performanceLogDownload");
function showPerformance(snapshot) {
  performanceSnapshot.hidden = !snapshot.enabled;
  if (!snapshot.enabled) return;
  performanceTimings.replaceChildren();
  for (const [label, prefix] of [["Main loop", "loop"], ["OLED refresh", "display"]]) {
    const row = performanceTimings.insertRow();
    for (const value of [label, ...["CurrentUs", "AverageUs", "PeakUs"].map(key =>
      (snapshot[prefix + key] / 1000).toFixed(2) + " ms"), snapshot[prefix + "Samples"]]) {
      const cell = row.insertCell();
      cell.textContent = value;
    }
  }
  performanceDetails.replaceChildren();
  for (const [label, value] of [
    ["Uptime", (snapshot.uptimeMs / 3600000).toFixed(1) + " h"],
    ["Largest loop gap", (snapshot.loopMaxGapUs / 1000).toFixed(2) + " ms"],
    ["Gaps over 100 ms", snapshot.loopGapsOver100ms],
    ["Internal RAM free / minimum", snapshot.internalFree + " / " + snapshot.internalMinimumFree + " B"],
    ["PSRAM free / minimum", snapshot.psramFree + " / " + snapshot.psramMinimumFree + " B"],
    ["NVS entries free", snapshot.nvsFreeEntries],
    ["ADC overflows", snapshot.adcOverflows], ["Live audio dropped frames", snapshot.liveAudioDroppedFrames],
  ]) {
    const term = document.createElement("dt"), detail = document.createElement("dd");
    term.textContent = label; detail.textContent = value;
    performanceDetails.append(term, detail);
  }
  performanceLogToggle.checked = snapshot.logEnabled;
  performanceLogDownload.hidden = !snapshot.logPath;
  performanceLogStatus.textContent = snapshot.logError ||
    (snapshot.logPath ? `${snapshot.logPath} (${snapshot.logSamples} samples, ${snapshot.logBytes} B, ${snapshot.logSkippedBusy} busy intervals skipped)` : "SD logging off");
}
async function loadPerformance() {
  const response = await fetch("/api/performance", { cache: "no-store" });
  if (!response.ok) throw Error(await response.text());
  const snapshot = await response.json();
  performanceToggle.checked = snapshot.enabled;
  showPerformance(snapshot);
  performanceStatus.textContent = snapshot.enabled ? "Diagnostics active" : "Diagnostics off";
  performanceToggle.disabled = false;
  performanceRefresh.disabled = false;
}
performanceLogToggle.addEventListener("change", async () => {
  performanceLogToggle.disabled = true;
  try {
    const response = await fetch("/api/performance/log?enabled=" + (performanceLogToggle.checked ? "1" : "0"), { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    await loadPerformance();
  } catch (error) {
    await loadPerformance().catch(() => {});
    performanceLogStatus.textContent = error.message || "Could not change SD logging.";
  } finally { performanceLogToggle.disabled = false; }
});
performanceRefresh.addEventListener("click", async () => {
  try { await loadPerformance(); }
  catch { performanceStatus.textContent = "Could not load diagnostics."; }
});
performanceToggle.addEventListener("change", async () => {
  performanceToggle.disabled = true;
  try {
    const response = await fetch("/api/performance?enabled=" + (performanceToggle.checked ? "1" : "0"), { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    await loadPerformance();
  } catch (error) {
    performanceStatus.textContent = error.message || "Could not save diagnostics setting.";
    await loadPerformance().catch(() => { performanceToggle.disabled = false; });
  }
});
loadPerformance().catch(() => { performanceStatus.textContent = "Could not load diagnostics."; });
const cueForm = document.querySelector("#cueSettingsForm"),
  cueSave = document.querySelector("#saveCueSettings"),
  cueResult = document.querySelector("#cueSettingsResult"),
  cueModes = ["manualRadio", "manualMic", "autoRadio", "autoMic"];
for (const mode of cueModes) for (const event of ["Start", "Stop"]) {
  document.querySelector("#" + mode + event).replaceChildren(
    new Option("Off", 0), new Option("Short", 1), new Option("Normal", 2));
}
async function loadCueSettings() {
  try {
    const response = await fetch("/api/record-cues", { cache: "no-store" });
    if (!response.ok) throw Error(await response.text());
    const settings = await response.json();
    for (const mode of cueModes) for (const event of ["Start", "Stop"])
      document.querySelector("#" + mode + event).value = settings[mode][event.toLowerCase()];
    cueSave.disabled = false;
    cueResult.textContent = "Sounds loaded.";
  } catch { cueResult.textContent = "Could not load recording sounds."; }
}
cueForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  cueSave.disabled = true;
  const settings = {};
  for (const mode of cueModes) settings[mode] = {
    start: Number(document.querySelector("#" + mode + "Start").value),
    stop: Number(document.querySelector("#" + mode + "Stop").value),
  };
  try {
    const response = await fetch("/api/record-cues", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(settings),
    });
    if (!response.ok) throw Error(await response.text());
    cueResult.textContent = "Sounds saved.";
  } catch (error) { cueResult.textContent = error.message || "Could not save recording sounds."; }
  finally { cueSave.disabled = false; }
});
loadCueSettings();
function updateSettingsNavigation() {
  const automatic = location.hash === "#autoSettings";
  for (const link of document.querySelectorAll(".auto-link")) {
    if (automatic) link.setAttribute("aria-current", "page");
    else link.removeAttribute("aria-current");
  }
  for (const link of document.querySelectorAll(".settings-link")) {
    if (automatic) link.removeAttribute("aria-current");
    else link.setAttribute("aria-current", "page");
  }
}
addEventListener("hashchange", updateSettingsNavigation);
updateSettingsNavigation();
const wifiStatus = document.querySelector("#wifiStatus"),
  scanButton = document.querySelector("#scanNetworks"),
  forgetButton = document.querySelector("#forgetNetwork"),
  networkList = document.querySelector("#networks"),
  connectForm = document.querySelector("#connectForm"),
  ssidInput = document.querySelector("#wifiSsid"),
  passwordInput = document.querySelector("#wifiPassword"),
  keepApInput = document.querySelector("#keepApHost");
const clockForm = document.querySelector("#clockForm"),
  clockLocal = document.querySelector("#clockLocal"),
  clockZone = document.querySelector("#clockZone"),
  clockStatus = document.querySelector("#clockStatus"),
  clockCurrent = document.querySelector("#clockCurrent");
const zoneLabels = ["Budapest (standard/daylight time)", "UTC"];
for (let offset = -12; offset <= 14; offset++)
  zoneLabels.push("UTC" + (offset >= 0 ? "+" : "") + offset);
zoneLabels.forEach((label, index) => clockZone.add(new Option(label, index)));
function formatUptime(seconds) {
  const days = Math.floor(seconds / 86400);
  return [Math.floor(days / 360), Math.floor((days % 360) / 30), days % 30]
    .map((value) => String(value).padStart(2, "0")).join(":") + " " +
    [Math.floor(seconds / 3600) % 24, Math.floor(seconds / 60) % 60, seconds % 60]
      .map((value) => String(value).padStart(2, "0")).join(":");
}
let uptimeBase = null,
  uptimeSyncedAt = performance.now();
function renderUptime() {
  document.querySelector("#runtime").textContent = uptimeBase === null ? "Unavailable" :
    formatUptime(uptimeBase + Math.floor((performance.now() - uptimeSyncedAt) / 1000));
}
setInterval(renderUptime, 1000);
function showClock(clock) {
  uptimeBase = Number.isFinite(clock.uptimeSeconds) && clock.uptimeSeconds >= 0 ?
    Math.floor(clock.uptimeSeconds) : null;
  uptimeSyncedAt = performance.now();
  renderUptime();
  clockZone.value = String(clock.zone);
  clockCurrent.textContent = clock.local ? clock.local.replace("T", " ") : "Time is not set";
  clockStatus.textContent = ({ internet: "Synchronized from NTP", manual: "Set manually",
    rtc: "Restored from RTC", phone: "Synchronized from phone",
    saved: "Saved time (without RTC)", default: "Default date (2000-01-01)",
    unset: "Time is not set" })[clock.source] || "Clock status unavailable";
  if (clock.local && !clockLocal.value) clockLocal.value = clock.local.slice(0, 16);
}
async function loadClock() {
  try {
    const response = await fetch("/api/time", { cache: "no-store" });
    if (!response.ok) throw Error();
    showClock(await response.json());
  } catch {
    clockStatus.textContent = "Clock status unavailable.";
  }
}
clockForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!clockForm.reportValidity()) return;
  const button = clockForm.querySelector("button");
  button.disabled = true;
  try {
    const response = await fetch("/api/time?" + new URLSearchParams({ local: clockLocal.value }), { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    showClock(await response.json());
  } catch (error) {
    clockStatus.textContent = error.message || "Could not set time.";
  } finally {
    button.disabled = false;
  }
});
clockZone.addEventListener("change", async () => {
  clockZone.disabled = true;
  try {
    const response = await fetch("/api/time/zone?" + new URLSearchParams({ zone: clockZone.value }), { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    clockLocal.value = "";
    showClock(await response.json());
  } catch (error) {
    clockStatus.textContent = error.message || "Could not save timezone.";
    await loadClock();
  } finally {
    clockZone.disabled = false;
  }
});
loadClock();
setInterval(loadClock, 10000);
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
    result.textContent = "Settings loaded.";
  } catch {
    result.textContent = "Could not load settings.";
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
  result.textContent = "Saving...";
  try {
    const params = new URLSearchParams({
      screenOffMinutes: minutes.value,
      wifiAutoOffMinutes: wifiMinutes.value,
    });
    const response = await fetch("/api/settings?" + params, { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    const settings = await response.json();
    result.textContent =
      "Saved · display " +
      settings.screenOffMinutes +
      " min · Wi-Fi " +
      settings.wifiAutoOffMinutes +
      " min";
  } catch (error) {
    result.textContent = error.message || "Save failed.";
  } finally {
    save.disabled = false;
  }
});
wakeDisplay.addEventListener("click", async () => {
  wakeDisplay.disabled = true;
  try {
    const response = await fetch("/api/display/wake", { method: "POST" });
    if (!response.ok) throw Error();
    result.textContent = "Display is awake.";
  } catch {
    result.textContent = "Could not wake display.";
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
    wifiStatus.textContent = "Scanning networks...";
    return;
  }
  if (data.state === "failed") {
    wifiStatus.textContent = "Scan failed.";
    return;
  }
  for (const network of data.networks || []) {
    const button = document.createElement("button");
    button.type = "button";
    const name = document.createElement("span");
    name.dataset.i18nIgnore = "";
    name.textContent = network.ssid;
    button.append(name, document.createTextNode(" · " + network.rssi + " dBm"),
      document.createTextNode(network.open ? " · open" : ""));
    button.addEventListener("click", () => {
      ssidInput.value = network.ssid;
      passwordInput.focus();
    });
    networkList.append(button);
  }
  if (!data.networks?.length) wifiStatus.textContent = "No networks found.";
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
    wifiStatus.textContent = error.message || "Scan failed.";
  } finally {
    scanButton.disabled = false;
  }
});
connectForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!connectForm.reportValidity()) return;
  document.querySelector("#connectWifi").disabled = true;
  wifiStatus.textContent = "Connecting to network...";
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
    wifiStatus.textContent = "Connection attempt started...";
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
    wifiStatus.textContent = error.message || "Could not start connection.";
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
    wifiStatus.textContent = "Network forgotten; AP mode active.";
    keepApInput.checked = false;
    passwordInput.value = "";
  } catch (error) {
    wifiStatus.textContent =
      error.message || "Could not forget network.";
  } finally {
    loadSettings();
  }
});
loadSettings();
