const folder = new URLSearchParams(location.search).get("dir") || "/";
fetch("/api/status", { cache: "no-store" })
  .then((response) => response.json())
  .then((data) => {
    document.querySelector("#firmwareVersion").textContent = "v" + data.version;
    demoBase = data.demoSeconds;
    clockSource = data.clock?.source || "unset";
    demoSyncedAt = performance.now();
    renderDemoClock();
  })
  .catch(() => {});
const listEl = document.querySelector("#items"),
  message = document.querySelector("#message"),
  more = document.querySelector("#more");
const speakerStatus = document.querySelector("#speakerStatus"),
  speakerStop = document.querySelector("#speakerStop");
const speakerVolume = document.querySelector("#speakerVolume"),
  speakerVolumeValue = document.querySelector("#speakerVolumeValue"),
  speakerBoost = document.querySelector("#speakerBoost");
const radioVolume = document.querySelector("#radioVolume"),
  radioVolumeValue = document.querySelector("#radioVolumeValue"),
  radioBoost = document.querySelector("#radioBoost"),
  playbackTime = document.querySelector("#playbackTime");
let next = 0,
  loading = false,
  loadFailed = false;
let volumeQueued = null,
  volumeSending = false,
  radioVolumeQueued = null,
  radioVolumeSending = false,
  boostUpdating = false,
  radioBoostUpdating = false;
let playbackRequestError = "";
let lastPlaybackFinished = false,
  wasPlaying = false,
  lastPlaybackPaused = false;
let demoBase = 0,
  demoSyncedAt = performance.now(),
  clockSource = "unset";
function renderDemoClock() {
  const seconds =
    (demoBase + Math.floor((performance.now() - demoSyncedAt) / 1000)) % 86400;
  document.querySelector("#demoClock").textContent =
    (clockSource === "unset" ? "TIME UNSET" :
      ({ internet: "NTP ", manual: "MANUAL ", rtc: "RTC ", phone: "PHONE ", saved: "SAVED ", default: "DEFAULT " })[clockSource] || "") +
    (clockSource === "unset" ? "" :
    [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60]
      .map((value) => String(value).padStart(2, "0"))
      .join(":"));
}
setInterval(renderDemoClock, 1000);
const sound = (name) =>
  /\.(wav|mp3|m4a|mp4|ogg|oga|opus|flac|aac|webm)$/i.test(name);
const link = (path) => "/files?dir=" + encodeURIComponent(path);
const playbackDialog = document.querySelector("#playbackDialog"),
  playbackTitle = document.querySelector("#playbackTitle"),
  playbackWaveform = document.querySelector("#playbackWaveform"),
  playbackSeek = document.querySelector("#playbackSeek"),
  playbackHead = document.querySelector("#playbackHead"),
  playbackPause = document.querySelector("#playbackPause"),
  browserAudio = document.querySelector("#browserAudio"),
  dialogVolume = document.querySelector("#dialogVolume"),
  dialogGain = document.querySelector("#dialogGain");
let playbackSession = null;
function playbackClock(seconds) {
  seconds = Math.max(0, Math.floor(seconds || 0));
  return Math.floor(seconds / 60) + ":" + String(seconds % 60).padStart(2, "0");
}
function updatePlayhead(fraction, duration) {
  const progress = Math.max(0, Math.min(1, fraction || 0));
  if (!playbackSeek.matches(":active")) playbackSeek.value = Math.round(progress * 1000);
  playbackHead.style.setProperty("--position", progress * 100 + "%");
  playbackTime.textContent = playbackClock(progress * duration) + " / " + playbackClock(duration);
}
function drawWaveform() {
  const width = playbackWaveform.clientWidth, height = playbackWaveform.clientHeight;
  if (!width) return;
  const ratio = devicePixelRatio || 1, context = playbackWaveform.getContext("2d");
  playbackWaveform.width = Math.round(width * ratio);
  playbackWaveform.height = Math.round(height * ratio);
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const peaks = playbackSession?.peaks || [];
  context.fillStyle = "#86c3b4";
  for (const [index, peak] of peaks.entries()) {
    const amplitude = Math.max(1, peak / 32768 * (height - 18));
    context.fillRect(index * width / peaks.length, (height - amplitude) / 2,
      Math.max(1, width / peaks.length - 1), amplitude);
  }
  context.strokeStyle = "#41736e";
  context.beginPath(); context.moveTo(0, height / 2); context.lineTo(width, height / 2); context.stroke();
}
async function loadWaveform(item, target, session) {
  try {
    if (/\.wav$/i.test(item.name)) {
      const response = await fetch("/api/play/waveform?path=" + encodeURIComponent(item.path), { cache: "no-store" });
      if (!response.ok) throw Error();
      const data = await response.json();
      session.peaks = data.peaks;
      session.duration = data.bytes / 44100;
    } else if (target === "browser" && item.bytes <= 12 * 1024 * 1024) {
      const response = await fetch("/file?path=" + encodeURIComponent(item.path));
      if (!response.ok) throw Error();
      const context = new AudioContext();
      try {
        const audio = await context.decodeAudioData(await response.arrayBuffer());
        session.duration = audio.duration;
        const channel = audio.getChannelData(0);
        session.peaks = Array.from({length:128}, (_, index) => {
          let peak = 0;
          const start = Math.floor(index * channel.length / 128);
          const end = Math.floor((index + 1) * channel.length / 128);
          for (let frame = start; frame < end; frame += Math.max(1, Math.floor((end - start) / 1024)))
            peak = Math.max(peak, Math.abs(channel[frame]));
          return peak * 32768;
        });
      } finally { await context.close(); }
    } else throw Error();
    if (playbackSession !== session) return;
    document.querySelector("#waveformStatus").textContent = "";
    drawWaveform();
  } catch {
    if (playbackSession === session) document.querySelector("#waveformStatus").textContent = "Waveform unavailable for this file.";
  }
}
function configureDialogVolume(target) {
  const volume = target === "radio" ? radioVolume : target === "speaker" ? speakerVolume : browserAudio;
  const gain = target === "radio" ? radioBoost : speakerBoost;
  document.querySelector('label[for="dialogVolume"]').textContent = target === "browser" ? "Browser volume" : target === "radio" ? "Radio DAC volume" : "ESP speaker volume";
  dialogVolume.value = target === "browser" ? Math.round(volume.volume * 100) : volume.value;
  document.querySelector("#dialogVolumeValue").textContent = dialogVolume.value + "%";
  document.querySelector("#dialogGainLabel").hidden = target === "browser";
  dialogGain.hidden = target === "browser";
  if (target !== "browser") {
    dialogGain.replaceChildren(...[...gain.options].map((option) => option.cloneNode(true)));
    dialogGain.value = gain.value;
  }
}
function openPlayback(item, target) {
  if (playbackDialog.open || (target !== "browser" && wasPlaying)) return;
  playbackSession = { path: item.path, target, peaks: [], duration: 0 };
  playbackTitle.textContent = item.name;
  speakerStatus.textContent = "Loading playback...";
  document.querySelector("#waveformStatus").textContent = "";
  playbackPause.disabled = false;
  speakerStop.disabled = false;
  playbackPause.textContent = "❚❚";
  playbackPause.setAttribute("aria-label", "Pause");
  playbackDialog.showModal();
  updatePlayhead(0, 0);
  drawWaveform();
  configureDialogVolume(target);
  const session = playbackSession;
  if (target === "browser") {
    browserAudio.src = "/file?path=" + encodeURIComponent(item.path);
    browserAudio.play().catch(() => { if (playbackSession === session) speakerStatus.textContent = "Browser playback unavailable."; });
    loadWaveform(item, target, session);
  } else {
    loadWaveform(item, target, session).then(async () => {
      if (playbackSession === session) {
        if (!await playOnEsp(item.path, target)) { playbackPause.disabled = true; speakerStop.disabled = true; }
      }
    });
  }
}
dialogVolume.addEventListener("input", () => {
  const value = Number(dialogVolume.value), target = playbackSession?.target;
  document.querySelector("#dialogVolumeValue").textContent = value + "%";
  if (target === "browser") browserAudio.volume = value / 100;
  if (target === "speaker") setSpeakerVolume(value);
  if (target === "radio") setRadioVolume(value);
});
dialogGain.addEventListener("change", () => {
  const gain = playbackSession?.target === "radio" ? radioBoost : speakerBoost;
  gain.value = dialogGain.value;
  gain.dispatchEvent(new Event("change"));
});
browserAudio.addEventListener("timeupdate", () => {
  if (playbackSession?.target === "browser" && Number.isFinite(browserAudio.duration))
    updatePlayhead(browserAudio.currentTime / browserAudio.duration, browserAudio.duration);
});
browserAudio.addEventListener("ended", () => {
  speakerStatus.textContent = "Playback finished";
  playbackPause.disabled = true; speakerStop.disabled = true;
});
browserAudio.addEventListener("playing", () => { speakerStatus.textContent = "Browser playback"; });
playbackSeek.addEventListener("change", async () => {
  const session = playbackSession;
  if (!session) return;
  const fraction = Number(playbackSeek.value) / 1000;
  if (session.target === "browser") {
    if (Number.isFinite(browserAudio.duration)) browserAudio.currentTime = fraction * browserAudio.duration;
  } else if (session.bytesTotal) {
    const offset = Math.min(session.bytesTotal - 2, Math.floor(fraction * session.bytesTotal / 2) * 2);
    const response = await fetch("/api/play/seek?offset=" + offset, { method: "POST" });
    if (!response.ok) speakerStatus.textContent = "Could not seek playback.";
  }
});
playbackSeek.addEventListener("input", () => {
  const fraction = Number(playbackSeek.value) / 1000;
  playbackHead.style.setProperty("--position", fraction * 100 + "%");
  playbackTime.textContent = playbackClock(fraction * (playbackSession?.duration || 0)) + " / " + playbackClock(playbackSession?.duration);
});
playbackPause.addEventListener("click", async () => {
  const target = playbackSession?.target;
  if (!target) return;
  if (target === "browser") {
    if (browserAudio.paused) await browserAudio.play().catch(() => {});
    else browserAudio.pause();
  } else {
    const response = await fetch("/api/play/pause?paused=" + (lastPlaybackPaused ? "0" : "1"), { method: "POST" });
    if (!response.ok) { speakerStatus.textContent = "Could not pause playback."; return; }
    await refreshSpeaker();
  }
  const paused = target === "browser" ? browserAudio.paused : lastPlaybackPaused;
  playbackPause.textContent = paused ? "▶" : "❚❚";
  playbackPause.setAttribute("aria-label", paused ? "Resume" : "Pause");
});
async function closePlayback() {
  if (playbackSession?.target === "browser") { browserAudio.pause(); browserAudio.removeAttribute("src"); browserAudio.load(); }
  else if (playbackSession && wasPlaying) {
    try {
      const response = await fetch("/api/play/stop", { method: "POST" });
      if (!response.ok && response.status !== 409) throw Error();
    } catch { speakerStatus.textContent = "Could not stop."; return; }
  }
  playbackSession = null;
  playbackDialog.close();
  refreshSpeaker();
}
document.querySelector("#playbackClose").addEventListener("click", closePlayback);
playbackDialog.addEventListener("cancel", (event) => { event.preventDefault(); closePlayback(); });
addEventListener("resize", () => { if (playbackDialog.open) drawWaveform(); });
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
async function sendRadioVolume() {
  if (radioVolumeSending) return;
  radioVolumeSending = true;
  while (radioVolumeQueued !== null) {
    const value = radioVolumeQueued;
    radioVolumeQueued = null;
    try {
      const response = await fetch("/api/play/radio-volume?value=" + value, { method: "POST" });
      if (!response.ok) throw Error();
    } catch {
      speakerStatus.textContent = "Could not change radio volume.";
    }
  }
  radioVolumeSending = false;
}
function setRadioVolume(value) {
  const volume = Math.max(0, Math.min(100, value));
  radioVolume.value = volume;
  radioVolumeValue.textContent = volume + "%";
  radioVolumeQueued = volume;
  sendRadioVolume();
}
radioVolume.addEventListener("input", () => setRadioVolume(Number(radioVolume.value)));
document.querySelector("#radioDown").addEventListener("click", () => setRadioVolume(Number(radioVolume.value) - 5));
document.querySelector("#radioUp").addEventListener("click", () => setRadioVolume(Number(radioVolume.value) + 5));
radioBoost.addEventListener("change", async () => {
  radioBoostUpdating = true;
  radioBoost.disabled = true;
  let failed = false;
  try {
    const response = await fetch("/api/play/radio-boost?value=" + radioBoost.value, { method: "POST" });
    if (!response.ok) throw Error();
  } catch {
    failed = true;
  }
  radioBoostUpdating = false;
  await refreshSpeaker();
  if (failed) speakerStatus.textContent = "Could not change radio gain.";
});
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
  await refreshSpeaker();
  if (failed) speakerStatus.textContent = "Could not change gain.";
});
async function refreshSpeaker() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw Error();
    const data = await response.json(),
      playing = data.playback !== "idle";
    if (wasPlaying && !playing && !data.playbackError) lastPlaybackFinished = true;
    if (playing) lastPlaybackFinished = false;
    wasPlaying = playing;
    lastPlaybackPaused = !!data.playbackPaused;
    demoBase = data.demoSeconds;
    clockSource = data.clock?.source || "unset";
    demoSyncedAt = performance.now();
    renderDemoClock();
    speakerStop.disabled = !playing && playbackSession?.target !== "browser";
    if (playbackSession?.target !== "browser") speakerStatus.textContent = playing
      ? (data.playbackTarget === "radio" ? "Radio DAC: " : "ESP speaker: ") +
        (data.playback === "starting" ? "Starting..." : data.playbackPath)
      : data.playbackError || playbackRequestError ||
        (lastPlaybackFinished ? "Playback finished" :
          data.radioReady === false ? "Radio DAC: unavailable" : "Playback: off");
    const bytesTotal = Number(data.playbackBytesTotal) || 0;
    const bytesPlayed = Math.min(bytesTotal, Number(data.playbackBytesPlayed) || 0);
    if (playbackSession && playbackSession.target !== "browser") {
      playbackSession.bytesTotal = bytesTotal;
      if (bytesTotal) playbackSession.duration = bytesTotal / 44100;
      if (playing && bytesTotal) updatePlayhead(bytesPlayed / bytesTotal, playbackSession.duration);
      playbackPause.disabled = !playing || data.playback === "starting";
      playbackPause.textContent = lastPlaybackPaused ? "▶" : "❚❚";
      playbackPause.setAttribute("aria-label", lastPlaybackPaused ? "Resume" : "Pause");
    }
    if (!volumeSending) {
      speakerVolume.value = data.speakerVolume;
      speakerVolumeValue.textContent = data.speakerVolume + "%";
    }
    const radioControlsReady = data.radioReady || (playing && data.playbackTarget === "radio");
    radioVolume.disabled = !radioControlsReady;
    radioBoost.disabled = !radioControlsReady || radioBoostUpdating;
    if (!radioBoostUpdating) radioBoost.value = String(data.radioBoost);
    if (!radioVolumeSending) {
      radioVolume.value = data.radioVolume;
      radioVolumeValue.textContent = data.radioVolume + "%";
    }
    if (playbackSession?.target === "speaker" || playbackSession?.target === "radio")
      if (!dialogVolume.matches(":active")) configureDialogVolume(playbackSession.target);
    speakerBoost.disabled = !data.speakerReady || boostUpdating;
    if (!boostUpdating) speakerBoost.value = String(data.speakerBoost);
    for (const button of listEl.querySelectorAll("[data-esp-play]"))
      button.disabled = playing || data.state !== "idle" || !data.speakerReady;
    for (const button of listEl.querySelectorAll("[data-browser-play]"))
      button.disabled = playing || !!playbackSession;
    for (const button of listEl.querySelectorAll("[data-radio-play]")) {
      button.disabled = playing || data.state !== "idle" || !data.radioReady;
      button.title = !data.radioReady
        ? "Radio DAC unavailable (I2S TX not initialized)"
        : "Play through the radio DAC";
    }
  } catch {
    if (playbackSession?.target !== "browser") speakerStatus.textContent = playbackRequestError || "ESP playback status unavailable.";
  }
}
async function playOnEsp(path, target = "speaker") {
  playbackRequestError = "";
  lastPlaybackFinished = false;
  try {
    const response = await fetch("/api/play?path=" + encodeURIComponent(path) + "&target=" + target, {
      method: "POST",
    });
    if (!response.ok) throw Error(await response.text());
    await refreshSpeaker();
    return true;
  } catch (error) {
    playbackRequestError = error.message || "Could not start ESP playback.";
    speakerStatus.textContent = playbackRequestError;
    return false;
  }
}
speakerStop.addEventListener("click", async () => {
  speakerStop.disabled = true;
  if (playbackSession?.target === "browser") {
    browserAudio.pause(); browserAudio.currentTime = 0;
    playbackPause.disabled = true;
    speakerStatus.textContent = "Playback stopped";
    return;
  }
  try {
    const response = await fetch("/api/play/stop", { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    playbackRequestError = "";
  } catch (error) {
    speakerStatus.textContent = error.message || "Could not stop.";
  }
  await refreshSpeaker();
});
setInterval(() => { if (playbackDialog.open && playbackSession?.target !== "browser") refreshSpeaker(); }, 500);
function breadcrumbs() {
  const crumbs = document.querySelector("#crumbs");
  let path = "";
  const root = document.createElement("li"),
    rootLink = document.createElement("a");
  rootLink.href = link("/");
  rootLink.textContent = "microSD";
  root.append(rootLink);
  crumbs.append(root);
  for (const name of folder.split("/").filter(Boolean)) {
    path += "/" + name;
    const part = document.createElement("li"),
      anchor = document.createElement("a");
    anchor.textContent = name;
    anchor.href = link(path);
    part.append(anchor);
    crumbs.append(part);
  }
  document.querySelector("#location").textContent = folder;
  const parent = document.querySelector("#parentFolder");
  parent.hidden = folder === "/";
  parent.href = link(folder.slice(0, folder.lastIndexOf("/")) || "/");
}
