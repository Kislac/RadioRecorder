const form = document.querySelector("#settingsForm"),
  minutes = document.querySelector("#screenOffMinutes"),
  wifiMinutes = document.querySelector("#wifiAutoOffMinutes"),
  save = document.querySelector("#save"),
  wakeDisplay = document.querySelector("#wakeDisplay"),
  result = document.querySelector("#result");
const performanceToggle = document.querySelector("#performanceEnabled"),
  performanceRefresh = document.querySelector("#refreshPerformance"),
  performanceStatus = document.querySelector("#performanceStatus"),
  performanceLive = document.querySelector("#performanceLive"),
  performanceTimings = document.querySelector("#performanceTimings"),
  performanceDetails = document.querySelector("#performanceDetails"),
  performanceLogToggle = document.querySelector("#performanceLogEnabled"),
  performanceLogStatus = document.querySelector("#performanceLogStatus"),
  performanceLogDownload = document.querySelector("#performanceLogDownload"),
  performanceLogFiles = document.querySelector("#performanceLogFiles"),
  performanceLogView = document.querySelector("#performanceLogView"),
  performanceLogMessage = document.querySelector("#performanceLogMessage"),
  performanceLogViewer = document.querySelector("#performanceLogViewer"),
  performanceLogRows = document.querySelector("#performanceLogRows"),
  performanceLogSummary = document.querySelector("#performanceLogSummary");
function usedPercent(free, total) {
  return Number.isFinite(free) && Number.isFinite(total) && total > 0 ?
    Math.min(100, Math.max(0, 100 * (total - free) / total)) : null;
}
function formatPercent(value) { return value === null ? "Unavailable" : value.toFixed(0) + "% used"; }
function severity(value, warning = 85, critical = 95) {
  return value === null ? "" : value >= critical ? "critical" : value >= warning ? "warning" : "";
}
function addMetric(label, value, note, level = "") {
  const row = document.createElement("div"), name = document.createElement("span"),
    help = document.createElement("abbr"), amount = document.createElement("strong");
  row.className = "metric-row" + (level ? " metric-" + level : "");
  name.textContent = label;
  help.textContent = "?";
  help.title = note;
  help.setAttribute("aria-label", note);
  help.tabIndex = 0;
  amount.textContent = (level === "critical" ? "Critical: " : level === "warning" ? "Watch: " : "") + value;
  name.append(" ", help);
  row.append(name, amount);
  performanceDetails.append(row);
}
function showPerformance(snapshot) {
  performanceLive.hidden = !snapshot.enabled;
  performanceLogToggle.disabled = !snapshot.enabled;
  if (!snapshot.enabled) {
    performanceLogToggle.checked = false;
    performanceLogStatus.textContent = "Diagnostics off; saved logs remain available.";
    return;
  }
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
  const uptimeMinutes = Math.floor(snapshot.uptimeMs / 60000);
  addMetric("Uptime", `${Math.floor(uptimeMinutes / 60)}h ${uptimeMinutes % 60}m`,
    "Time since the ESP last started. This counter resets after a restart.");
  addMetric("Longest loop gap", (snapshot.loopMaxGapUs / 1000).toFixed(1) + " ms",
    "Longest interval between main-loop starts since diagnostics were enabled. A large value can indicate blocking work.",
    snapshot.loopMaxGapUs >= 1000000 ? "critical" : snapshot.loopMaxGapUs >= 250000 ? "warning" : "");
  const slowRate = snapshot.loopSamples ? 100 * snapshot.loopGapsOver100ms / snapshot.loopSamples : 0;
  addMetric("Loops delayed over 100 ms", `${snapshot.loopGapsOver100ms} (${slowRate.toFixed(1)}%)`,
    "Share of main-loop intervals above 100 ms since diagnostics were enabled. Watch above 1%, critical above 5%.",
    severity(slowRate, 1, 5));
  for (const [label, prefix] of [["Internal RAM", "internal"], ["PSRAM", "psram"]]) {
    const current = usedPercent(snapshot[prefix + "Free"], snapshot[prefix + "Total"]);
    const worst = usedPercent(snapshot[prefix + "MinimumFree"], snapshot[prefix + "Total"]);
    addMetric(label, `${formatPercent(current)} · peak ${formatPercent(worst)}`,
      `Free now: ${snapshot[prefix + "Free"]} B; lowest free since boot: ${snapshot[prefix + "MinimumFree"]} B; total allocatable: ${snapshot[prefix + "Total"]} B. Amber at 85%, red at 95%; these are advisory thresholds, not hardware limits.`,
      severity(worst));
  }
  const nvsUse = usedPercent(snapshot.nvsFreeEntries, snapshot.nvsTotalEntries);
  addMetric("NVS settings storage", `${formatPercent(nvsUse)} · ${snapshot.nvsFreeEntries} entries free`,
    "Flash entries available for saved settings, not SD space or bytes. Amber at 85%, red at 95% used.", severity(nvsUse));
  addMetric("ADC buffer overruns", String(snapshot.adcOverflows),
    "Audio samples discarded when the radio input buffer could not be drained in time.",
    snapshot.adcOverflows ? "warning" : "");
  addMetric("Live audio dropped frames", String(snapshot.liveAudioDroppedFrames),
    "Frames not sent to web listeners when the live audio queue was full.",
    snapshot.liveAudioDroppedFrames ? "warning" : "");
  if (snapshot.logSamples) addMetric("Last SD log write", (snapshot.logWriteUs / 1000).toFixed(1) + " ms",
    "Time to open, append and close the latest diagnostics log sample on SD. Watch above 250 ms, critical above 1 s.",
    severity(snapshot.logWriteUs / 1000, 250, 1000));
  performanceLogToggle.checked = snapshot.logEnabled;
  performanceLogDownload.hidden = !snapshot.logPath;
  performanceLogStatus.textContent = snapshot.logError ||
    (snapshot.logPath ? `${snapshot.logEnabled ? "Recording" : "Stopped"}: ${snapshot.logPath} (${snapshot.logSamples} samples, ${snapshot.logBytes} B, ${snapshot.logSkippedBusy} busy intervals skipped)` : "SD logging off");
}
async function loadLogFiles() {
  const selected = performanceLogFiles.value;
  const response = await fetch("/api/performance/logs", { cache: "no-store" });
  if (!response.ok) throw Error(await response.text());
  const data = await response.json();
  performanceLogFiles.replaceChildren(...data.files.map(file => new Option(`${file.path.slice(1)} (${Math.ceil(file.bytes / 1024)} KiB)`, file.path)));
  if (data.files.some(file => file.path === selected)) performanceLogFiles.value = selected;
  performanceLogView.disabled = !performanceLogFiles.value;
  performanceLogDownload.hidden = !performanceLogFiles.value;
  if (performanceLogFiles.value) performanceLogDownload.href = "/api/performance/log?path=" + encodeURIComponent(performanceLogFiles.value);
  performanceLogMessage.textContent = data.files.length ? `${data.files.length} saved log(s)` : "No saved logs";
}
performanceLogFiles.addEventListener("change", () => {
  performanceLogViewer.hidden = true;
  performanceLogDownload.href = "/api/performance/log?path=" + encodeURIComponent(performanceLogFiles.value);
});
performanceLogView.addEventListener("click", async () => {
  const path = performanceLogFiles.value;
  if (!path) return;
  performanceLogView.disabled = true;
  try {
    const response = await fetch("/api/performance/log?path=" + encodeURIComponent(path), { cache: "no-store" });
    if (!response.ok) throw Error(await response.text());
    const lines = (await response.text()).trim().split("\n").filter(Boolean);
    if (lines.length > 512) throw Error("Log contains too many samples to display.");
    performanceLogRows.replaceChildren();
    for (const line of lines) {
      const sample = JSON.parse(line), metrics = sample.metrics;
      if (!metrics || !metrics.enabled) throw Error("Invalid diagnostics sample.");
      const row = performanceLogRows.insertRow();
      for (const text of [sample.timestampUtc, ...["loop", "display"].map(prefix =>
        `${(metrics[prefix + "AverageUs"] / 1000).toFixed(1)} / ${(metrics[prefix + "PeakUs"] / 1000).toFixed(1)} ms`),
      formatPercent(usedPercent(metrics.internalFree, metrics.internalTotal)),
      formatPercent(usedPercent(metrics.psramFree, metrics.psramTotal)),
      String(metrics.adcOverflows + metrics.liveAudioDroppedFrames)]) row.insertCell().textContent = text;
    }
    performanceLogSummary.textContent = `${path} · ${lines.length} sample(s)`;
    performanceLogMessage.textContent = lines.length ? "" : "This log has no samples yet.";
    performanceLogViewer.hidden = !lines.length;
  } catch (error) {
    performanceLogViewer.hidden = true;
    performanceLogMessage.textContent = error.message || "Could not read log.";
  } finally { performanceLogView.disabled = false; }
});
async function loadPerformance() {
  const response = await fetch("/api/performance", { cache: "no-store" });
  if (!response.ok) throw Error(await response.text());
  const snapshot = await response.json();
  performanceToggle.checked = snapshot.enabled;
  showPerformance(snapshot);
  performanceStatus.textContent = snapshot.enabled ? "Diagnostics active" : "Diagnostics off";
  performanceToggle.disabled = false;
  performanceRefresh.disabled = false;
  await loadLogFiles().catch(error => { performanceLogMessage.textContent = error.message || "Log list unavailable."; });
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
