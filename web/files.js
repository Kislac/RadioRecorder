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
  playbackProgressWrap = document.querySelector("#playbackProgressWrap"),
  playbackProgress = document.querySelector("#playbackProgress"),
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
  wasPlaying = false;
let demoBase = 0,
  demoSyncedAt = performance.now(),
  clockSource = "unset";
function renderDemoClock() {
  const seconds =
    (demoBase + Math.floor((performance.now() - demoSyncedAt) / 1000)) % 86400;
  document.querySelector("#demoClock").textContent =
    (clockSource === "unset" ? "TIME UNSET" :
      ({ internet: "NTP ", manual: "MANUAL ", rtc: "RTC ", saved: "SAVED ", default: "DEFAULT " })[clockSource] || "") +
    (clockSource === "unset" ? "" :
    [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60]
      .map((value) => String(value).padStart(2, "0"))
      .join(":"));
}
setInterval(renderDemoClock, 1000);
const sound = (name) =>
  /\.(wav|mp3|m4a|mp4|ogg|oga|opus|flac|aac|webm)$/i.test(name);
const link = (path) => "/files?dir=" + encodeURIComponent(path);
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
    demoBase = data.demoSeconds;
    clockSource = data.clock?.source || "unset";
    demoSyncedAt = performance.now();
    renderDemoClock();
    speakerStop.disabled = !playing;
    speakerStatus.textContent = playing
      ? (data.playbackTarget === "radio" ? "Radio DAC: " : "ESP speaker: ") +
        (data.playback === "starting" ? "Starting..." : data.playbackPath)
      : data.playbackError || playbackRequestError ||
        (lastPlaybackFinished ? "Playback finished" :
          data.radioReady === false ? "Radio DAC: unavailable" : "Playback: off");
    const bytesTotal = Number(data.playbackBytesTotal) || 0;
    const bytesPlayed = Math.min(bytesTotal, Number(data.playbackBytesPlayed) || 0);
    playbackProgressWrap.hidden = !playing || !bytesTotal;
    playbackProgress.max = Math.max(bytesTotal, 1);
    playbackProgress.value = bytesPlayed;
    const playbackClock = (bytes) => {
      const seconds = Math.floor(bytes / (22050 * 2));
      return Math.floor(seconds / 60) + ":" + String(seconds % 60).padStart(2, "0");
    };
    playbackTime.textContent = playbackClock(bytesPlayed) + " / " + playbackClock(bytesTotal);
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
    speakerBoost.disabled = !data.speakerReady || boostUpdating;
    if (!boostUpdating) speakerBoost.value = String(data.speakerBoost);
    for (const button of listEl.querySelectorAll("[data-esp-play]"))
      button.disabled = playing || data.state !== "idle" || !data.speakerReady;
    for (const button of listEl.querySelectorAll("[data-radio-play]")) {
      button.disabled = playing || data.state !== "idle" || !data.radioReady;
      button.title = !data.radioReady
        ? "Radio DAC unavailable (I2S TX not initialized)"
        : "Play through the radio DAC";
    }
  } catch {
    speakerStatus.textContent = playbackRequestError || "ESP playback status unavailable.";
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
  } catch (error) {
    playbackRequestError = error.message || "Could not start ESP playback.";
    speakerStatus.textContent = playbackRequestError;
  }
}
speakerStop.addEventListener("click", async () => {
  speakerStop.disabled = true;
  try {
    const response = await fetch("/api/play/stop", { method: "POST" });
    if (!response.ok) throw Error(await response.text());
    playbackRequestError = "";
  } catch (error) {
    speakerStatus.textContent = error.message || "Could not stop.";
  }
  await refreshSpeaker();
});
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
