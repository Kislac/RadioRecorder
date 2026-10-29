const readRadio = document.getElementById("readRadio");
const wiring = document.getElementById("wiring");
const radioStatus = document.getElementById("radioStatus");
const progress = document.getElementById("progress");
const result = document.getElementById("result");
const image = document.getElementById("image");
const exportButton = document.getElementById("export");
let busy = false;
let snapshot = null;
let timer;

const mhz = value => value == null ? "—" : (value / 1000000).toFixed(5);
const dtcs = [23,25,26,31,32,36,43,47,51,53,54,65,71,72,73,74,114,115,116,122,125,131,132,134,143,145,152,155,156,162,165,172,174,205,212,223,225,226,243,244,245,246,251,252,255,261,263,265,266,271,274,306,311,315,325,331,332,343,346,351,356,364,365,371,411,412,413,423,431,432,445,446,452,454,455,462,464,465,466,503,506,516,523,526,532,546,565,606,612,624,627,631,632,645,654,662,664,703,712,723,731,732,734,743,754];
function tone(value) {
  if (value === 0 || value === 65535) return "—";
  if (value >= 600) return `${(value / 10).toFixed(1)} Hz`;
  const reverse = value >= 106;
  const code = dtcs[value - (reverse ? 106 : 1)];
  return code === undefined ? `raw ${value}` : `D${String(code).padStart(3, "0")}${reverse ? "R" : "N"}`;
}
function showError(error) {
  radioStatus.textContent = error.message;
  radioStatus.classList.add("error");
}
async function request(path, options) {
  const response = await fetch(path, { cache: "no-store", ...options, signal: AbortSignal.timeout(5000) });
  if (!response.ok) throw new Error(await response.text());
  return response.json();
}
function render(state) {
  busy = state.state === "reading";
  readRadio.disabled = busy || !wiring.checked;
  wiring.disabled = busy;
  progress.value = state.bytesRead;
  progress.max = state.totalBytes;
  radioStatus.classList.toggle("error", state.state === "error");
  radioStatus.textContent = state.state === "error" ? state.error : busy ? `Kiolvasás: ${state.bytesRead} / ${state.totalBytes} bájt` : state.state === "done" ? `Kiolvasás kész · ${state.firmware}` : "Nincs kiolvasott rádió.";
  result.hidden = image.hidden = exportButton.hidden = state.state !== "done";
  if (state.state !== "done") { snapshot = null; return; }
  snapshot = state;
  const settings = document.getElementById("settings");
  settings.replaceChildren();
  const data = state.settings;
  const values = { Firmware: state.firmware, "Squelch": data.squelch, "Energiatakarékosság (nyers)": data.save, "VOX": data.vox, "Háttérvilágítás (nyers)": data.backlight, "Dual Watch": data.dualWatch ? "Be" : "Ki", "Gombhang": data.beep ? "Be" : "Ki", "Adásidőkorlát (nyers)": data.timeoutRaw, "A csatorna": data.channelA, "B csatorna": data.channelB, "VFO A · MHz": mhz(data.vfoAHz), "VFO B · MHz": mhz(data.vfoBHz) };
  for (const [label, value] of Object.entries(values)) {
    const term = document.createElement("dt");
    const description = document.createElement("dd");
    term.textContent = label;
    description.textContent = value;
    settings.append(term, description);
  }
  const channels = document.getElementById("channels");
  channels.replaceChildren();
  document.getElementById("count").textContent = `(${state.channels.length}/128)`;
  for (const channel of state.channels) {
    const row = document.createElement("tr");
    for (const value of [channel.number, channel.name, mhz(channel.rxHz), mhz(channel.txHz), tone(channel.rxToneRaw), tone(channel.txToneRaw), channel.bandwidth, `raw ${channel.powerRaw}`, channel.scan ? "Igen" : "Nem"]) {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.append(cell);
    }
    channels.append(row);
  }
}
async function poll() {
  clearTimeout(timer);
  try { render(await request("/api/radio")); }
  catch (error) { showError(error); }
  timer = setTimeout(poll, busy ? 500 : 3000);
}
wiring.addEventListener("change", () => { readRadio.disabled = busy || !wiring.checked; });
readRadio.addEventListener("click", async () => {
  if (busy || !wiring.checked) return;
  clearTimeout(timer);
  busy = true;
  readRadio.disabled = wiring.disabled = true;
  result.hidden = image.hidden = exportButton.hidden = true;
  snapshot = null;
  try { render(await request("/api/radio/read?confirm=1", { method: "POST" })); }
  catch (error) { showError(error); }
  timer = setTimeout(poll, 500);
});
exportButton.addEventListener("click", () => {
  if (!snapshot) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(snapshot, null, 2)], { type: "application/json" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = "uv5r-settings.json";
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
poll();