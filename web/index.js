const stateEl = document.querySelector("#state"),
  toggle = document.querySelector("#toggle"),
  micToggle = document.querySelector("#micToggle"),
  lowpassToggle = document.querySelector("#lowpassToggle"),
  message = document.querySelector("#message"),
  firmwareVersion = document.querySelector("#firmwareVersion");
const speakerStatus = document.querySelector("#speakerStatus"),
  speakerStop = document.querySelector("#speakerStop");
const speakerVolume = document.querySelector("#speakerVolume"),
  speakerVolumeValue = document.querySelector("#speakerVolumeValue"),
  speakerBoost = document.querySelector("#speakerBoost");
const signalToggle = document.querySelector("#signalToggle"),
  signalValues = document.querySelector("#signalValues");
const signalPlot = document.querySelector("#signalPlot"),
  signalChart = document.querySelector("#signalChart"),
  signalHeading = document.querySelector("#signalHeading");
const micSignalToggle = document.querySelector("#micSignalToggle"),
  micSignalValues = document.querySelector("#micSignalValues"),
  micSignalPlot = document.querySelector("#micSignalPlot"),
  micSignalChart = document.querySelector("#micSignalChart");
const liveToggle = document.querySelector("#liveToggle"),
  liveStatus = document.querySelector("#liveStatus"),
  liveVolume = document.querySelector("#liveVolume"),
  liveTest = document.querySelector("#liveTest");
let currentState = "", currentRecordingMode = "none", recordingRequestError = "";
let meterRunning = false,
  meterGeneration = 0,
  meterTimer = null,
  lastSignalSequence = -1;
let micMeterRunning = false,
  micMeterGeneration = 0,
  micMeterTimer = null,
  lastMicSequence = -1;
let liveSocket = null,
  audioContext = null,
  audioGain = null,
  nextAudioTime = 0;
let volumeQueued = null,
  volumeSending = false,
  boostUpdating = false;
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
async function sendSpeakerVolume() {
  if (volumeSending) return;
  volumeSending = true;
  while (volumeQueued !== null) {
    const value = volumeQueued;
    volumeQueued = null;
    try {
      const response = await fetch("/api/play/volume?value=" + value, {
        method: "POST",
      });
      if (!response.ok) throw Error();
    } catch {
      speakerStatus.textContent = "Could not change volume.";
    }
  }
  volumeSending = false;
}
function setSpeakerVolume(value) {
  const volume = Math.max(0, Math.min(100, value));
  speakerVolume.value = volume;
  speakerVolumeValue.textContent = volume + "%";
  volumeQueued = volume;
  sendSpeakerVolume();
}
speakerVolume.addEventListener("input", () =>
  setSpeakerVolume(Number(speakerVolume.value)),
);
document
  .querySelector("#speakerDown")
  .addEventListener("click", () =>
    setSpeakerVolume(Number(speakerVolume.value) - 5),
  );
document
  .querySelector("#speakerUp")
  .addEventListener("click", () =>
    setSpeakerVolume(Number(speakerVolume.value) + 5),
  );
speakerBoost.addEventListener("change", async () => {
  boostUpdating = true;
  speakerBoost.disabled = true;
  let failed = false;
  try {
    const response = await fetch(
      "/api/play/boost?value=" + speakerBoost.value,
      { method: "POST" },
    );
    if (!response.ok) throw Error();
  } catch {
    failed = true;
  }
  boostUpdating = false;
  await refresh();
  if (failed) speakerStatus.textContent = "Could not change gain.";
});
async function playOnEsp(path) {
  try {
    const response = await fetch("/api/play?path=" + encodeURIComponent(path), {
      method: "POST",
    });
    if (!response.ok) throw Error(await response.text());
    await refresh();
  } catch (error) {
    speakerStatus.textContent = error.message || "Could not start ESP playback.";
  }
}
speakerStop.addEventListener("click", async () => {
  speakerStop.disabled = true;
  try {
    const response = await fetch("/api/play/stop", { method: "POST" });
    if (!response.ok) throw Error(await response.text());
  } catch (error) {
    speakerStatus.textContent = error.message || "Could not stop.";
  }
  await refresh();
});
function stopLive() {
  const socket = liveSocket;
  liveSocket = null;
  if (socket) socket.close();
  const context = audioContext;
  audioContext = null;
  audioGain = null;
  if (context) context.close().catch(() => {});
  liveToggle.textContent = "Start listening";
  liveStatus.textContent = "Disabled";
}
liveToggle.addEventListener("click", async () => {
  if (liveSocket) {
    stopLive();
    return;
  }
  liveToggle.disabled = true;
  try {
    const Context = window.AudioContext || window.webkitAudioContext;
    if (!Context || !window.WebSocket) throw Error();
    audioContext = new Context({ latencyHint: "interactive" });
    audioGain = audioContext.createGain();
    audioGain.gain.value = Number(liveVolume.value) / 100;
    audioGain.connect(audioContext.destination);
    const unlock = audioContext.createBufferSource();
    unlock.buffer = audioContext.createBuffer(1, 1, 22050);
    unlock.connect(audioGain);
    unlock.start();
    await audioContext.resume();
    const socket = new WebSocket(
      window.radioRecorderAudioUrl || "ws://" + location.hostname + ":81/",
    );
    socket.binaryType = "arraybuffer";
    liveSocket = socket;
    liveStatus.textContent = "Connecting...";
    socket.onopen = () => {
      if (socket !== liveSocket) return;
      liveToggle.textContent = "Stop listening";
      liveStatus.textContent = "Waiting for audio...";
    };
    socket.onmessage = (event) => {
      if (
        socket !== liveSocket ||
        !(event.data instanceof ArrayBuffer) ||
        !audioContext
      )
        return;
      const view = new DataView(event.data),
        length = Math.floor(view.byteLength / 2);
      if (!length) return;
      const buffer = audioContext.createBuffer(1, length, 22050),
        channel = buffer.getChannelData(0);
      let peak = 0;
      for (let index = 0; index < length; index++) {
        const value = view.getInt16(index * 2, true) / 32768;
        channel[index] = value;
        peak = Math.max(peak, Math.abs(value));
      }
      if (audioContext.state !== "running") {
        liveStatus.textContent =
          "Playback paused (" +
          audioContext.state +
          "), PCM peak: " +
          Math.round(peak * 100) +
          "%";
        return;
      }
      const now = audioContext.currentTime;
      if (nextAudioTime < now + 0.12 || nextAudioTime > now + 0.8)
        nextAudioTime = now + 0.12;
      const source = audioContext.createBufferSource();
      source.buffer = buffer;
      source.connect(audioGain);
      source.start(nextAudioTime);
      nextAudioTime += buffer.duration;
      liveStatus.textContent =
        "Live audio, PCM peak: " +
        Math.round(peak * 100) +
        "%, kb. " +
        Math.round((nextAudioTime - now) * 1000) +
        " ms puffer";
    };
    socket.onclose = () => {
      if (socket !== liveSocket) return;
      stopLive();
      liveStatus.textContent = "Connection closed.";
    };
    socket.onerror = () => {
      if (socket !== liveSocket) return;
      stopLive();
      liveStatus.textContent = "Audio connection unavailable.";
    };
  } catch {
    stopLive();
    liveStatus.textContent = "The browser could not start audio.";
  }
  liveToggle.disabled = false;
});
liveVolume.addEventListener("input", () => {
  if (audioGain) audioGain.gain.value = Number(liveVolume.value) / 100;
});
liveTest.addEventListener("click", async () => {
  const Context = window.AudioContext || window.webkitAudioContext;
  if (!Context) {
    liveStatus.textContent = "Audio test unsupported.";
    return;
  }
  const context = audioContext || new Context({ latencyHint: "interactive" });
  try {
    const tone = context.createOscillator(),
      gain = context.createGain();
    tone.frequency.value = 440;
    gain.gain.value = (Number(liveVolume.value) / 100) * 0.15;
    tone.connect(gain);
    gain.connect(context.destination);
    tone.onended = () => {
      tone.disconnect();
      gain.disconnect();
      if (context !== audioContext) context.close().catch(() => {});
    };
    tone.start();
    tone.stop(context.currentTime + 0.4);
    await context.resume();
    liveStatus.textContent = "Audio test played.";
  } catch {
    if (context !== audioContext) context.close().catch(() => {});
    liveStatus.textContent = "Audio test failed.";
  }
});
addEventListener("pagehide", stopLive);
function drawSignal(chart = signalChart, history = signalHistory, ceiling = 4096) {
  const width = chart.clientWidth,
    height = 200,
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
            Math.max(1, ...history.map((point) => point.peak)) * 1.2,
          ),
        ),
    ),
  );
  context.font = '12px "Trebuchet MS",sans-serif';
  context.lineWidth = 1;
  context.textAlign = "right";
  for (let step = 0; step <= 4; step++) {
    const ordinate = bottom - ((bottom - top) * step) / 4;
    context.strokeStyle = "#d8e0d9";
    context.beginPath();
    context.moveTo(left, ordinate);
    context.lineTo(right, ordinate);
    context.stroke();
    context.fillStyle = "#52635a";
    context.fillText(
      String(Math.round((maximum * step) / 4)),
      left - 7,
      ordinate + 4,
    );
  }
  for (const [key, color] of [
    ["rms", "#12604f"],
    ["peak", "#ad3d34"],
  ]) {
    context.strokeStyle = color;
    context.lineWidth = 2;
    context.beginPath();
    history.forEach((point, index) => {
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
      signalPlot.hidden = true;
      signalToggle.textContent = "Enable measurement";
      signalValues.textContent = data.fault
        ? "ADC measurement error (" + data.fault + ")"
        : "Disabled";
      return;
    }
    signalToggle.textContent = "Disable measurement";
    signalPlot.hidden = false;
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
      signalPlot.hidden = true;
      signalValues.textContent = "Disabled";
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
      micSignalPlot.hidden = true;
      micSignalToggle.textContent = "Enable measurement";
      micSignalValues.textContent = data.fault
        ? "Microphone measurement error (" + data.fault + ")"
        : "Disabled";
      return;
    }
    micSignalToggle.textContent = "Disable measurement";
    micSignalPlot.hidden = false;
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
      micSignalPlot.hidden = true;
      micSignalValues.textContent = "Disabled";
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
    if (window.renderAutomaticRecording) window.renderAutomaticRecording(data.automatic);
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
    const ownsRadio = data.recordingMode === "manualRadio", ownsMic = data.recordingMode === "manualMic";
    toggle.textContent = busy && ownsRadio ? "Stop recording" : "Start recording";
    micToggle.textContent = busy && ownsMic ? "Stop microphone recording" : "Start microphone recording";
    toggle.classList.toggle("recording", busy && data.recordSource === "adc");
    micToggle.classList.toggle("recording", busy && data.recordSource === "microphone");
    toggle.disabled =
      (!data.ready && !busy) ||
      data.state === "starting" ||
      (busy && data.stopping) ||
      playing || (!busy && data.fileBusy);
    micToggle.disabled =
      (!data.micReady && !busy) ||
      data.state === "starting" ||
      (busy && data.stopping) ||
      playing || (!busy && (!!liveSocket || data.fileBusy));
    speakerStop.disabled = !playing;
    speakerStatus.textContent = playing
      ? (data.playbackTarget === "radio" ? "Radio DAC: " : "ESP speaker: ") +
        (data.playback === "starting" ? "Starting..." : data.playbackPath)
      : data.playbackError || "ESP speaker: off";
    if (!volumeSending) {
      speakerVolume.value = data.speakerVolume;
      speakerVolumeValue.textContent = data.speakerVolume + "%";
    }
    speakerBoost.disabled = !data.speakerReady || boostUpdating;
    if (!boostUpdating) speakerBoost.value = String(data.speakerBoost);
    signalToggle.disabled = (!data.meterReady || micMeterRunning) && !meterRunning;
    micSignalToggle.disabled = (!data.micReady || meterRunning || !!liveSocket) && !micMeterRunning;
    liveToggle.disabled = (!data.liveReady || playing) && !liveSocket;
    liveTest.disabled = !!data.automatic?.microphone?.armed;
    message.textContent = data.error || recordingRequestError;
    currentState = data.state;
    currentRecordingMode = data.recordingMode || "none";
  } catch {
    stateEl.textContent = "Connection lost";
    toggle.disabled = true;
  }
}
toggle.addEventListener("click", async () => {
  recordingRequestError = "";
  toggle.disabled = true;
  try {
    const response = await fetch(
      currentState !== "idle" && currentRecordingMode === "manualRadio" ? "/api/stop" : "/api/start",
      { method: "POST" },
    );
    if (!response.ok) recordingRequestError = await response.text();
  } catch {
    recordingRequestError = "Request failed.";
  }
  await refresh();
});
micToggle.addEventListener("click", async () => {
  recordingRequestError = "";
  micToggle.disabled = true;
  try {
    const response = await fetch(currentState !== "idle" && currentRecordingMode === "manualMic" ? "/api/stop" : "/api/mic/start", { method: "POST" });
    if (!response.ok) recordingRequestError = await response.text();
  } catch {
    recordingRequestError = "Microphone request failed.";
  }
  await refresh();
});
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
