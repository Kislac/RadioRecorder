const stateEl = document.querySelector("#state"),
  recordAction = document.querySelector("#recordAction"),
  modeInputs = [...document.querySelectorAll('input[name="recordMode"]')],
  sourceInputs = [...document.querySelectorAll('input[name="recordSource"]')],
  lowpassToggle = document.querySelector("#lowpassToggle"),
  message = document.querySelector("#message"),
  firmwareVersion = document.querySelector("#firmwareVersion");
const signalToggle = document.querySelector("#signalToggle"),
  signalValues = document.querySelector("#signalValues");
const signalPlot = document.querySelector("#signalPlot"),
  signalChart = document.querySelector("#signalChart"),
  signalHeading = document.querySelector("#signalHeading");
const micSignalToggle = document.querySelector("#micSignalToggle"),
  micSignalValues = document.querySelector("#micSignalValues"),
  micSignalPlot = document.querySelector("#micSignalPlot"),
  micSignalChart = document.querySelector("#micSignalChart");
let recordingRequestError = "";
let lastStatus = null, userSelectedMode = false, autoMeterSource = null, recordActionPending = false;
let lastArchiveFetch = 0;
const selectedMode = () => modeInputs.find((input) => input.checked).value;
const selectedSource = () => sourceInputs.find((input) => input.checked).value;
function renderRecordAction(data) {
  const busy = data.state !== "idle";
  const mode = selectedMode(), source = selectedSource();
  document.querySelector(".home-watch-head span").textContent = source === "radio" ? "RADIO / SIGNAL MONITOR" : "MICROPHONE / SIGNAL MONITOR";
  const sourceCode = source === "radio" ? "Radio" : "Mic";
  const owner = (mode === "manual" ? "manual" : "auto") + sourceCode;
  const armed = !!data.automatic?.[source]?.armed;
  recordAction.textContent = mode === "manual"
    ? busy && data.recordingMode === owner ? source === "radio" ? "Stop recording" : "Stop microphone recording" : source === "radio" ? "Start recording" : "Start microphone recording"
    : armed ? "Stop automatic monitoring" : "Start automatic monitoring";
  recordAction.classList.toggle("recording", mode === "manual" ? busy && data.recordingMode === owner : armed);
  recordAction.disabled = recordActionPending || (mode === "manual"
    ? (busy && data.recordingMode !== owner) || (!busy && !(source === "radio" ? data.ready : data.micReady)) || data.state === "starting" || (busy && data.stopping) || data.playback !== "idle" || (!busy && data.fileBusy)
    : !armed && (busy || data.fileBusy || data.playback !== "idle" || !(source === "radio" ? data.ready : data.micReady)));
  for (const input of [...modeInputs, ...sourceInputs]) input.disabled = busy || recordActionPending;
  for (const panel of document.querySelectorAll(".signal-panel")) panel.hidden = panel.dataset.source !== source;
}
for (const input of [...modeInputs, ...sourceInputs]) input.addEventListener("change", () => {
  userSelectedMode = true;
  document.querySelector("#monitorState").textContent = "NO LIVE DATA";
  if (lastStatus) {
    renderRecordAction(lastStatus);
  }
  if (selectedSource() === "radio") drawSignal();
  else drawSignal(micSignalChart, micSignalHistory, 32768);
  resetSpectrum();
});
recordAction.addEventListener("click", async () => {
  if (recordAction.disabled) return;
  recordActionPending = true;
  recordAction.disabled = true;
  recordingRequestError = "";
  const source = selectedSource();
  try {
    const url = selectedMode() === "manual"
      ? lastStatus?.state !== "idle" ? "/api/stop" : source === "radio" ? "/api/start" : "/api/mic/start"
      : "/api/auto-record/arm?" + new URLSearchParams({ source, enabled: lastStatus?.automatic?.[source]?.armed ? "0" : "1" });
    const response = await fetch(url, { method: "POST" });
    if (!response.ok) throw Error(await response.text());
  } catch (error) {
    recordingRequestError = error.message || "Request failed.";
  }
  recordActionPending = false;
  await refresh();
});
let meterRunning = false,
  meterGeneration = 0,
  meterTimer = null,
  lastSignalSequence = -1;
let micMeterRunning = false,
  micMeterGeneration = 0,
  micMeterTimer = null,
  lastMicSequence = -1;
let clockBase = NaN,
  demoSyncedAt = performance.now(),
  clockSource = "unset";
function renderDemoClock() {
  const clock = document.querySelector("#demoClock");
  if (clockSource === "unset" || !Number.isFinite(clockBase)) {
    clock.textContent = "TIME UNSET";
    clock.removeAttribute("datetime");
    return;
  }
  const elapsed = Math.floor((performance.now() - demoSyncedAt) / 1000) * 1000;
  const local = new Date(clockBase + elapsed).toISOString().slice(0, 19);
  clock.dateTime = local;
  clock.textContent =
    (({ internet: "NTP ", manual: "MANUAL ", rtc: "RTC ", saved: "SAVED ", default: "DEFAULT " })[clockSource] || "") +
    local.slice(0, 10).replaceAll("-", ".") + ". " + local.slice(11);
}
setInterval(renderDemoClock, 1000);
const signalHistory = [];
const micSignalHistory = [];
const spectrumBands = [...document.querySelectorAll(".home-spectrum-bands span")];
const spectrumFrequencies = [125, 250, 500, 1000, 2000, 4000, 8000, 10000];
let spectrumPending = false;
function resetSpectrum() {
  for (const band of spectrumBands) {
    band.style.removeProperty("--level");
    band.style.removeProperty("--bar-color");
  }
  document.querySelector("#spectrumState").textContent = "NO PCM DATA";
}
function drawSpectrum(buffer) {
  if (buffer.byteLength !== 1024) return resetSpectrum();
  const view = new DataView(buffer);
  const samples = 512;
  const levels = spectrumFrequencies.map((frequency) => {
    let real = 0, imaginary = 0;
    for (let index = 0; index < samples; index++) {
      const window = 0.5 - 0.5 * Math.cos(2 * Math.PI * index / (samples - 1));
      const phase = 2 * Math.PI * frequency * index / 22050;
      const value = view.getInt16(index * 2, true) * window;
      real += value * Math.cos(phase);
      imaginary += value * Math.sin(phase);
    }
    return Math.hypot(real, imaginary);
  });
  const strongest = Math.max(...levels);
  for (const [index, band] of spectrumBands.entries()) {
    const relative = strongest > 1 ? 20 * Math.log10(Math.max(1, levels[index]) / strongest) : -60;
    band.style.setProperty("--level", Math.max(2, Math.round(80 * (relative + 60) / 60)) + "px");
    band.style.setProperty("--bar-color", "#86c3b4");
  }
  document.querySelector("#spectrumState").textContent = "LIVE PCM";
}
async function pollSpectrum() {
  if (spectrumPending || !lastStatus) return;
  const source = selectedSource();
  if (lastStatus.state === "idle" && !(source === "radio" ? meterRunning : micMeterRunning)) {
    resetSpectrum();
    return;
  }
  spectrumPending = true;
  try {
    const response = await fetch("/api/spectrum?source=" + source, { cache: "no-store" });
    if (source !== selectedSource()) return;
    if (response.status === 204) resetSpectrum();
    else if (response.ok) drawSpectrum(await response.arrayBuffer());
    else resetSpectrum();
  } catch { resetSpectrum(); }
  finally { spectrumPending = false; }
}
setInterval(pollSpectrum, 400);
function drawSignal(chart = signalChart, history = signalHistory, ceiling = 4096) {
  const width = chart.clientWidth,
    height = 160,
    ratio = devicePixelRatio || 1,
    context = chart.getContext("2d");
  chart.width = Math.round(width * ratio);
  chart.height = Math.round(height * ratio);
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const left = 42,
    right = width - 12,
    top = 15,
    bottom = height - 28;
  const maximum = Math.min(
    ceiling,
    Math.max(
      128,
      2 **
        Math.ceil(
          Math.log2(
            Math.max(1, ...history.map((point) => point.peak ?? point.rms)) * 1.2,
          ),
        ),
    ),
  );
  context.font = '12px "IBM Plex Mono",sans-serif';
  context.lineWidth = 1;
  context.textAlign = "right";
  for (let step = 0; step <= 4; step++) {
    const ordinate = bottom - ((bottom - top) * step) / 4;
    context.strokeStyle = "#41736e";
    context.beginPath();
    context.moveTo(left, ordinate);
    context.lineTo(right, ordinate);
    context.stroke();
    context.fillStyle = "#acc9c2";
    context.fillText(
      String(Math.round((maximum * step) / 4)),
      left - 7,
      ordinate + 4,
    );
  }
  context.textAlign = "center";
  context.fillStyle = "#acc9c2";
  for (let seconds = 0; seconds <= 30; seconds += 10) {
    context.fillText((seconds - 30) + "s", left + (right - left) * seconds / 30, height - 7);
  }
  for (const [key, color] of [
    ["rms", "#86c3b4"],
    ["peak", "#e1ba7b"],
  ]) {
    context.strokeStyle = color;
    context.lineWidth = 2;
    context.beginPath();
    history.forEach((point, index) => {
      if (point[key] == null) return;
      const x =
        right - ((history.length - 1 - index) * (right - left)) / 119;
      const y =
        bottom - (Math.min(maximum, point[key]) * (bottom - top)) / maximum;
      if (index) context.lineTo(x, y);
      else context.moveTo(x, y);
    });
    context.stroke();
  }
}
async function pollSignal(generation) {
  if (!meterRunning || generation !== meterGeneration) return;
  try {
    const response = await fetch("/api/signal", { cache: "no-store" });
    if (!response.ok) throw Error();
    const data = await response.json();
    if (generation !== meterGeneration) return;
    if (!data.enabled) {
      meterRunning = false;
      signalPlot.hidden = false;
      signalToggle.textContent = "Enable measurement";
      if (selectedSource() === "radio") document.querySelector("#monitorState").textContent = "NO LIVE DATA";
      signalValues.textContent = data.fault
        ? "ADC measurement error (" + data.fault + ")"
        : "Disabled";
      return;
    }
    signalToggle.textContent = "Disable measurement";
    signalPlot.hidden = false;
    if (selectedSource() === "radio") document.querySelector("#monitorState").textContent = data.active && data.sequence > 0 ? "LIVE DATA" : "NO LIVE DATA";
    if (
      data.active &&
      data.sequence !== lastSignalSequence &&
      data.sequence > 0
    ) {
      lastSignalSequence = data.sequence;
      signalHistory.push({ rms: data.rms, peak: data.peak });
      if (signalHistory.length > 120) signalHistory.shift();
      drawSignal();
    }
    signalValues.textContent =
      data.active && data.sequence > 0
        ? "RMS: " +
          data.rms +
          " | Peak: " +
          data.peak +
          " | DC offset: " +
          data.bias +
          " (ADC)"
        : "Starting measurement...";
  } catch {
    signalValues.textContent = "Measurement data unavailable.";
  } finally {
    if (meterRunning && generation === meterGeneration)
      meterTimer = setTimeout(() => pollSignal(generation), 250);
  }
}
signalToggle.addEventListener("click", async () => {
  signalToggle.disabled = true;
  const turnOn = !meterRunning,
    previous = meterRunning;
  ++meterGeneration;
  clearTimeout(meterTimer);
  try {
    const response = await fetch(
      turnOn ? "/api/signal/on" : "/api/signal/off",
      { method: "POST" },
    );
    if (!response.ok) throw Error();
    meterRunning = turnOn;
    if (turnOn) {
      signalHistory.length = 0;
      lastSignalSequence = -1;
      signalPlot.hidden = false;
      drawSignal();
      pollSignal(meterGeneration);
    } else {
      signalPlot.hidden = false;
      signalValues.textContent = signalHistory.length ? "Last measured RMS: " + signalHistory.at(-1).rms + " | Peak: " + signalHistory.at(-1).peak : "Disabled";
      signalToggle.textContent = "Enable measurement";
    }
  } catch {
    meterRunning = previous;
    signalValues.textContent = "Could not toggle measurement.";
  }
  signalToggle.disabled = false;
});
async function pollMicSignal(generation) {
  if (!micMeterRunning || generation !== micMeterGeneration) return;
  try {
    const response = await fetch("/api/mic/signal", { cache: "no-store" });
    if (!response.ok) throw Error();
    const data = await response.json();
    if (generation !== micMeterGeneration) return;
    if (!data.enabled) {
      micMeterRunning = false;
      micSignalPlot.hidden = false;
      micSignalToggle.textContent = "Enable measurement";
      if (selectedSource() === "microphone") document.querySelector("#monitorState").textContent = "NO LIVE DATA";
      micSignalValues.textContent = data.fault
        ? "Microphone measurement error (" + data.fault + ")"
        : "Disabled";
      return;
    }
    micSignalToggle.textContent = "Disable measurement";
    micSignalPlot.hidden = false;
    if (selectedSource() === "microphone") document.querySelector("#monitorState").textContent = data.active && data.sequence > 0 ? "LIVE DATA" : "NO LIVE DATA";
    if (data.active && data.sequence > 0 && data.sequence !== lastMicSequence) {
      lastMicSequence = data.sequence;
      micSignalHistory.push({ rms: data.rms, peak: data.peak });
      if (micSignalHistory.length > 120) micSignalHistory.shift();
      drawSignal(micSignalChart, micSignalHistory, 32768);
    }
    micSignalValues.textContent = data.active && data.sequence > 0
      ? "RMS: " + data.rms + " | Peak: " + data.peak + " (PCM)"
      : "Starting measurement...";
  } catch {
    micSignalValues.textContent = "Microphone measurement unavailable.";
  } finally {
    if (micMeterRunning && generation === micMeterGeneration)
      micMeterTimer = setTimeout(() => pollMicSignal(generation), 250);
  }
}
micSignalToggle.addEventListener("click", async () => {
  micSignalToggle.disabled = true;
  const turnOn = !micMeterRunning,
    previous = micMeterRunning;
  ++micMeterGeneration;
  clearTimeout(micMeterTimer);
  try {
    const response = await fetch(turnOn ? "/api/mic/signal/on" : "/api/mic/signal/off", { method: "POST" });
    if (!response.ok) throw Error();
    micMeterRunning = turnOn;
    if (turnOn) {
      micSignalHistory.length = 0;
      lastMicSequence = -1;
      micSignalPlot.hidden = false;
      drawSignal(micSignalChart, micSignalHistory, 32768);
      pollMicSignal(micMeterGeneration);
    } else {
      micSignalPlot.hidden = false;
      micSignalValues.textContent = micSignalHistory.length ? "Last measured RMS: " + micSignalHistory.at(-1).rms + " | Peak: " + micSignalHistory.at(-1).peak : "Disabled";
      micSignalToggle.textContent = "Enable measurement";
    }
  } catch {
    micMeterRunning = previous;
    micSignalValues.textContent = "Could not toggle microphone measurement.";
  }
  micSignalToggle.disabled = false;
});
addEventListener("pagehide", () => {
  if (meterRunning) navigator.sendBeacon("/api/signal/off");
  if (micMeterRunning) navigator.sendBeacon("/api/mic/signal/off");
});
addEventListener("resize", () => {
  if (meterRunning) drawSignal();
  if (micMeterRunning) drawSignal(micSignalChart, micSignalHistory, 32768);
});
async function refreshArchive() {
  try {
    const [recordingsResponse, storageResponse] = await Promise.all([
      fetch("/api/recordings?offset=0", { cache: "no-store" }),
      fetch("/api/files?dir=%2F", { cache: "no-store" }),
    ]);
    if (!recordingsResponse.ok || !storageResponse.ok) throw Error();
    const recordings = await recordingsResponse.json();
    const storage = await storageResponse.json();
    const list = document.querySelector("#recentRecordings");
    list.replaceChildren();
    for (const file of recordings.files.slice(0, 3)) {
      const link = document.createElement("a"), label = document.createElement("span"), detail = document.createElement("small");
      link.href = "/file?path=" + encodeURIComponent(file.path);
      label.textContent = file.name;
      detail.textContent = (file.bytes / 1024).toFixed(1) + " KiB";
      label.append(detail); link.append(label); list.append(link);
    }
    if (!list.childElementCount) list.textContent = "No recordings yet.";
    const free = storage.capacity - storage.used;
    document.querySelector("#homeStorage").textContent = storage.capacity > 0
      ? (free / (1024 ** 3)).toFixed(1) + " GiB FREE" : "UNAVAILABLE";
  } catch {
    document.querySelector("#recentRecordings").textContent = "Recordings unavailable.";
    document.querySelector("#homeStorage").textContent = "UNAVAILABLE";
  }
}
async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw Error();
    const data = await response.json();
    firmwareVersion.textContent = "v" + data.version;
    clockBase = data.clock?.local ? Date.parse(data.clock.local + "Z") : NaN;
    clockSource = data.clock?.source || "unset";
    demoSyncedAt = performance.now();
    renderDemoClock();
    document.querySelector("#homeRtc").textContent = clockSource === "unset" ? "UNSET" : clockSource.toUpperCase();
    document.querySelector("#homeAp").textContent = data.wifi?.apSsid && data.wifi?.apIp
      ? data.wifi.apSsid + " / " + data.wifi.apIp : "OFF";
    document.querySelector("#homeSta").textContent = data.wifi?.staSsid && data.wifi?.staIp
      ? data.wifi.staSsid + " / " + data.wifi.staIp : "OFF";
    if (data.state === "idle" && data.ready && !data.fileBusy && Date.now() - lastArchiveFetch > 15000) {
      lastArchiveFetch = Date.now();
      refreshArchive();
    }
    if (window.renderAutomaticRecording) window.renderAutomaticRecording(data.automatic);
    if (data.state !== "idle" && data.recordingMode && data.recordingMode !== "none") {
      const source = data.recordingMode.endsWith("Mic") ? "microphone" : "radio";
      const mode = data.recordingMode.startsWith("auto") ? "auto" : "manual";
      sourceInputs.find((input) => input.value === source).checked = true;
      modeInputs.find((input) => input.value === mode).checked = true;
    } else if (!userSelectedMode && (data.automatic?.radio?.armed || data.automatic?.microphone?.armed)) {
      modeInputs.find((input) => input.value === "auto").checked = true;
      sourceInputs.find((input) => input.value === (data.automatic.radio.armed ? "radio" : "microphone")).checked = true;
    }
    const elapsed = Math.floor((data.recordElapsedMs || 0) / 1000);
    document.querySelector("#recordElapsed").textContent = "Time: " + [Math.floor(elapsed / 3600), Math.floor(elapsed / 60) % 60, elapsed % 60].map(value => String(value).padStart(2,"0")).join(":");
    lowpassToggle.checked = data.lowpass;
    signalHeading.textContent = "GPIO" + data.audioGpio + " signal";
    signalChart.setAttribute(
      "aria-label",
      "GPIO" + data.audioGpio + " RMS and peak history",
    );
    const busy = data.state !== "idle";
    const playing = data.playback !== "idle";
    stateEl.textContent = !data.ready && !data.micReady
      ? "SD or audio input unavailable"
      : busy
        ? data.state === "starting"
          ? "Starting recording..."
          : data.recordSource === "microphone" ? "Microphone recording in progress" : "ADC recording in progress"
        : "Ready to record";
    signalToggle.disabled = (!data.meterReady || micMeterRunning) && !meterRunning;
    micSignalToggle.disabled = (!data.micReady || meterRunning) && !micMeterRunning;
    message.textContent = data.error || recordingRequestError;
    lastStatus = data;
    renderRecordAction(data);
    const source = selectedSource();
    const monitoring = data.state !== "idle" || (selectedMode() === "auto" && !!data.automatic?.[source]?.armed);
    if (autoMeterSource && autoMeterSource !== source) {
      const oldMeter = autoMeterSource === "radio" ? signalToggle : micSignalToggle;
      if (autoMeterSource === "radio" ? meterRunning : micMeterRunning) oldMeter.click();
      autoMeterSource = null;
    }
    if (monitoring && autoMeterSource !== source) {
      autoMeterSource = source;
      const meter = source === "radio" ? signalToggle : micSignalToggle;
      if (!meterRunning && !micMeterRunning && !meter.disabled) meter.click();
    } else if (!monitoring && autoMeterSource) {
      const meter = autoMeterSource === "radio" ? signalToggle : micSignalToggle;
      if (autoMeterSource === "radio" ? meterRunning : micMeterRunning) meter.click();
      autoMeterSource = null;
    }
    if (monitoring && !(source === "radio" ? meterRunning : micMeterRunning)) {
      const sample = data.automatic?.[source];
      if (sample?.armed && Number.isFinite(sample.rms)) {
        const history = source === "radio" ? signalHistory : micSignalHistory;
        const plot = source === "radio" ? signalPlot : micSignalPlot;
        const chart = source === "radio" ? signalChart : micSignalChart;
        history.push({ rms: sample.rms, peak: null });
        if (history.length > 120) history.shift();
        plot.hidden = false;
        drawSignal(chart, history, source === "radio" ? 4096 : 32768);
        (source === "radio" ? signalValues : micSignalValues).textContent = "RMS: " + sample.rms + " | Peak: unavailable";
      }
    }
  } catch {
    stateEl.textContent = "Connection lost";
    recordAction.disabled = true;
  }
}
lowpassToggle.addEventListener("change", async () => {
  const enabled = lowpassToggle.checked;
  lowpassToggle.disabled = true;
  try {
    const response = await fetch(
      enabled ? "/api/filter/on" : "/api/filter/off",
      { method: "POST" },
    );
    if (!response.ok) throw Error();
  } catch {
    lowpassToggle.checked = !enabled;
    message.textContent = "Could not toggle low-pass filter.";
  }
  await refresh();
  lowpassToggle.disabled = false;
});
drawSignal();
refresh();
setInterval(refresh, 1500);
fetch("/api/signal", { cache: "no-store" })
  .then((response) => response.json())
  .then((data) => {
    if (data.enabled) {
      meterRunning = true;
      signalToggle.textContent = "Disable measurement";
      signalPlot.hidden = false;
      pollSignal(meterGeneration);
    }
  })
  .catch(() => {});
fetch("/api/mic/signal", { cache: "no-store" })
  .then((response) => response.json())
  .then((data) => {
    if (data.enabled) {
      micMeterRunning = true;
      micSignalToggle.textContent = "Disable measurement";
      micSignalPlot.hidden = false;
      pollMicSignal(micMeterGeneration);
    }
  })
  .catch(() => {});
