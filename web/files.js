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
let next = 0,
  loading = false,
  loadFailed = false;
let volumeQueued = null,
  volumeSending = false,
  boostUpdating = false;
let demoBase = 0,
  demoSyncedAt = performance.now(),
  clockSource = "unset";
function renderDemoClock() {
  const seconds =
    (demoBase + Math.floor((performance.now() - demoSyncedAt) / 1000)) % 86400;
  document.querySelector("#demoClock").textContent =
    (clockSource === "unset" ? "IDŐ NINCS" :
      ({ internet: "NTP ", manual: "KÉZI ", saved: "MENTETT ", default: "ALAP " })[clockSource] || "") +
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
      speakerStatus.textContent = "A hangerő módosítása sikertelen.";
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
  await refreshSpeaker();
  if (failed) speakerStatus.textContent = "Az erősítés módosítása sikertelen.";
});
async function refreshSpeaker() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw Error();
    const data = await response.json(),
      playing = data.playback !== "idle";
    demoBase = data.demoSeconds;
    clockSource = data.clock?.source || "unset";
    demoSyncedAt = performance.now();
    renderDemoClock();
    speakerStop.disabled = !playing;
    speakerStatus.textContent = playing
      ? "ESP hangszóró: " +
        (data.playback === "starting" ? "indul..." : data.playbackPath)
      : data.playbackError || "ESP hangszóró: kikapcsolva";
    if (!volumeSending) {
      speakerVolume.value = data.speakerVolume;
      speakerVolumeValue.textContent = data.speakerVolume + "%";
    }
    speakerBoost.disabled = !data.speakerReady || boostUpdating;
    if (!boostUpdating) speakerBoost.value = String(data.speakerBoost);
    for (const button of listEl.querySelectorAll("[data-esp-play]"))
      button.disabled = playing || data.state !== "idle" || !data.speakerReady;
  } catch {
    speakerStatus.textContent = "ESP-lejátszás állapota nem elérhető.";
  }
}
async function playOnEsp(path) {
  try {
    const response = await fetch("/api/play?path=" + encodeURIComponent(path), {
      method: "POST",
    });
    if (!response.ok) throw Error(await response.text());
    await refreshSpeaker();
  } catch (error) {
    speakerStatus.textContent = error.message || "ESP-lejátszás nem indult.";
  }
}
speakerStop.addEventListener("click", async () => {
  speakerStop.disabled = true;
  try {
    const response = await fetch("/api/play/stop", { method: "POST" });
    if (!response.ok) throw Error(await response.text());
  } catch (error) {
    speakerStatus.textContent = error.message || "Leállítás sikertelen.";
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
async function deleteEntry(item) {
  const kind = item.directory ? "mappát és minden tartalmát" : "fájlt";
  if (!confirm(`Biztosan törlöd a(z) ${item.name} ${kind}? Ez nem vonható vissza.`)) return;
  try {
    const response = await fetch("/api/files/delete?path=" + encodeURIComponent(item.path) + "&confirm=1", {
      method: "POST",
    });
    if (!response.ok) throw Error(await response.text());
    await load(true);
  } catch (error) {
    message.textContent = "Törlés sikertelen: " + error.message;
  }
}
function actionButton(label, title, action) {
  const button = document.createElement("button");
  button.type = "button";
  button.textContent = label;
  button.title = title;
  button.addEventListener("click", action);
  return button;
}
function render(item) {
  const row = document.createElement("li"),
    info = document.createElement("div"),
    name = document.createElement("div");
  name.className = "name";
  if (item.directory) {
    const anchor = document.createElement("a");
    anchor.href = link(item.path);
    anchor.textContent = item.name;
    name.append(anchor);
  } else name.textContent = item.name;
  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = item.directory
    ? "Mappa"
    : (item.bytes / 1024).toFixed(0) + " kB";
  info.append(name, meta);
  row.append(info);
  const actions = document.createElement("div");
  actions.className = "actions";
  if (item.directory) {
    const download = document.createElement("a");
    download.href = "/api/files/download?path=" + encodeURIComponent(item.path);
    download.textContent = "ZIP letöltése";
    download.title = "Mappa letöltése ZIP fájlban";
    actions.append(download);
  } else {
    if (sound(item.name)) {
      const radio = actionButton("Rádión", "Rádiós lejátszás később", () => {});
      radio.disabled = true;
      actions.append(radio);
      const play = actionButton("Telefonon", "Lejátszás ezen a készüléken", () => {
        const existing = info.querySelector("audio");
        if (existing) {
          existing.pause();
          existing.remove();
          play.textContent = "Telefonon";
          return;
        }
        const audio = document.createElement("audio");
        audio.controls = true;
        audio.src = "/file?path=" + encodeURIComponent(item.path);
        info.append(audio);
        play.textContent = "Bezárás";
        audio.play().catch(() => {});
      });
      actions.append(play);
    }
    if (/\.wav$/i.test(item.name)) {
      const espPlay = document.createElement("button");
      espPlay.type = "button";
      espPlay.dataset.espPlay = item.path;
      espPlay.textContent = "ESP-n lejátszás";
      espPlay.title = "Lejátszás az ESP hangszóróján";
      espPlay.addEventListener("click", () => playOnEsp(item.path));
      actions.append(espPlay);
    }
    const download = document.createElement("a");
    download.href =
      "/file?path=" + encodeURIComponent(item.path) + "&download=1";
    download.download = item.name;
    download.textContent = "Letöltés";
    actions.append(download);
  }
  actions.append(actionButton("Törlés", "Végleges törlés megerősítéssel", () => deleteEntry(item)));
  row.append(actions);
  listEl.append(row);
}
async function load(reset = false) {
  if (loading) return;
  loading = true;
  if (reset) {
    listEl.replaceChildren();
    next = 0;
  }
  loadFailed = false;
  try {
    const response = await fetch(
      "/api/files?dir=" + encodeURIComponent(folder) + "&offset=" + next,
      { cache: "no-store" },
    );
    if (!response.ok)
      throw Error(
        response.status === 409
          ? "Felvétel alatt a fájlok nem olvashatók."
          : "A mappa nem elérhető.",
      );
    const data = await response.json();
    data.items.forEach(render);
    next = data.next;
    refreshSpeaker();
    more.hidden = next === null;
    more.textContent = "Továbbiak";
    message.textContent = listEl.children.length ? "" : "Üres mappa.";
  } catch (error) {
    message.textContent = error.message;
    loadFailed = true;
    more.hidden = false;
    more.textContent = "Újrapróbálás";
  } finally {
    loading = false;
  }
  if (!loadFailed && next !== null) requestAnimationFrame(() => {
    if (!loading && !loadFailed && more.getBoundingClientRect().top <= innerHeight + 240) load();
  });
}
document.querySelector("#reload").addEventListener("click", () => load(true));
more.addEventListener("click", () => load());
if ("IntersectionObserver" in window) {
  const observer = new IntersectionObserver((entries) => {
    if (entries[0].isIntersecting && !loading && !loadFailed && next !== null) load();
  }, { rootMargin: "0px 0px 240px 0px" });
  observer.observe(more);
}
breadcrumbs();
load();
setInterval(refreshSpeaker, 1500);
