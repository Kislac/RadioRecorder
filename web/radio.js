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
  if (value == null) return "Nem exportált";
  if (value === 0 || value === 65535) return "—";
  if (value >= 600) return `${(value / 10).toFixed(1)} Hz`;
  const reverse = value >= 106;
  const code = dtcs[value - (reverse ? 106 : 1)];
  return code === undefined ? `raw ${value}` : `D${String(code).padStart(3, "0")}${reverse ? "R" : "N"}`;
}
const raw = value => value == null ? "Nem exportált" : `Nyers: ${value}`;
const option = (value, choices) => Number.isInteger(value) && value >= 0 && value < choices.length ? choices[value] : raw(value);
const onOff = value => option(value, ["Ki", "Be"]);
const bounded = (value, maximum, format) => Number.isInteger(value) && value >= 0 && value <= maximum ? format(value) : raw(value);
const pttId = value => option(value, ["Ki", "BOT", "EOT", "BOTH"]);
const power = value => option(value, ["HIGH", "LOW"]);
function renderDefinitions(element, values) {
  element.replaceChildren();
  for (const [label, value] of values) {
    const term = document.createElement("dt");
    const description = document.createElement("dd");
    term.textContent = label;
    description.textContent = value == null ? "Nem exportált" : value;
    element.append(term, description);
  }
}
function addRow(table, values) {
  const row = document.createElement("tr");
  for (const value of values) {
    const cell = document.createElement("td");
    cell.textContent = value;
    row.append(cell);
  }
  table.append(row);
}
function renderVfos(state) {
  const body = document.getElementById("vfos");
  body.replaceChildren();
  const version = Number(state.firmware.match(/BFB(\d{3})/)?.[1]);
  const steps = version && version < 291 ? [2.5, 5, 6.25, 10, 12.5, 25] : [2.5, 5, 6.25, 10, 12.5, 20, 25, 50];
  const rows = [
    ["Frekvencia · MHz", data => mhz(data.frequencyHz)],
    ["1 STEP · kHz", data => option(data.stepRaw, steps)],
    ["2 TXP", data => power(data.powerRaw)],
    ["5 W/N", data => option(data.bandwidthRaw, ["Wide", "Narrow"])],
    ["10 R-DCS", data => data.rxToneRaw == null ? raw(null) : data.rxToneRaw > 0 && data.rxToneRaw < 600 ? tone(data.rxToneRaw) : "Ki"],
    ["11 R-CTCS", data => data.rxToneRaw == null ? raw(null) : data.rxToneRaw >= 600 && data.rxToneRaw < 65535 ? tone(data.rxToneRaw) : "Ki"],
    ["12 T-DCS", data => data.txToneRaw == null ? raw(null) : data.txToneRaw > 0 && data.txToneRaw < 600 ? tone(data.txToneRaw) : "Ki"],
    ["13 T-CTCS", data => data.txToneRaw == null ? raw(null) : data.txToneRaw >= 600 && data.txToneRaw < 65535 ? tone(data.txToneRaw) : "Ki"],
    ["17 S-CODE", data => bounded(data.signalCodeRaw, 14, value => value + 1)],
    ["25 SFT-D", data => option(data.shiftRaw, ["OFF", "+", "-"])],
    ["26 OFFSET · MHz", data => mhz(data.offsetHz)],
    ["33 BAND", data => option(data.bandRaw, ["VHF", "UHF"])]
  ];
  for (const [label, format] of rows) addRow(body, [label, format(state.vfoA || {}), format(state.vfoB || {})]);
}
function renderCodes(state) {
  const dtmf = state.dtmf || {};
  const startup = state.startup || {};
  const values = [
    ["ANI-ID", dtmf.aniCode?.trim() || (dtmf.aniCode == null ? "Nem exportált" : "Üres")],
    ["Riasztási kód", dtmf.alarmCode?.trim() || (dtmf.alarmCode == null ? "Nem exportált" : "Üres")],
    ["ANI küldés", pttId(dtmf.aniModeRaw)],
    ["DTMF hangidő", bounded(dtmf.toneOnRaw, 195, value => `${50 + value * 10} ms`)],
    ["DTMF szünet", bounded(dtmf.toneOffRaw, 195, value => `${50 + value * 10} ms`)],
    ["Indítási üzenet 1", startup.line1], ["Indítási üzenet 2", startup.line2],
    ["6 + bekapcsolás 1", startup.alternateLine1], ["6 + bekapcsolás 2", startup.alternateLine2]
  ];
  for (let index = 1; index <= 15; ++index) values.push([`S-CODE ${index}`, dtmf[`code${index}`]?.trim() || (dtmf[`code${index}`] == null ? "Nem exportált" : "Üres")]);
  renderDefinitions(document.getElementById("codes"), values);
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
  busy = state.state === "reading" || state.state === "writing";
  readRadio.disabled = busy || !wiring.checked;
  wiring.disabled = busy;
  progress.value = state.stage === "write" ? state.bytesWritten : state.bytesRead;
  progress.max = state.stage === "write" ? state.writeBytes : state.totalBytes;
  radioStatus.classList.toggle("error", state.state === "error");
  const stage = state.stage === "preflight" ? "Előzetes összehasonlítás" : state.stage === "write" ? "Írás" : state.stage === "verify" ? "Visszaellenőrzés" : "Kiolvasás";
  radioStatus.textContent = state.state === "error" ? state.error : busy ? `${stage}: ${progress.value} / ${progress.max} bájt` : state.state === "done" ? `${state.writeVerified ? "Írás visszaellenőrizve" : "Kiolvasás kész"} · ${state.firmware}` : "Nincs kiolvasott rádió.";
  if (typeof radioEditorUpdate === "function") radioEditorUpdate(state);
  result.hidden = image.hidden = exportButton.hidden = state.state !== "done";
  if (state.state !== "done") { snapshot = null; return; }
  snapshot = state;
  const settings = document.getElementById("settings");
  const data = state.settings;
  const version = Number(state.firmware.match(/BFB(\d{3})/)?.[1]);
  const voice = version && version < 251 ? ["Ki", "Be"] : ["Ki", "Angol", "Kínai"];
  const values = [
    ["Firmware", state.firmware], ["0 SQL", data.squelch],
    ["3 SAVE", option(data.save, ["Ki", "1:1", "1:2", "1:3", "1:4"])],
    ["4 VOX", bounded(data.vox, 10, value => value ? String(value) : "Ki")],
    ["6 ABR", bounded(data.backlight, 24, value => value ? `${value} s` : "Ki")],
    ["7 TDR", onOff(data.dualWatch)], ["8 BEEP", onOff(data.beep)],
    ["9 TOT", bounded(data.timeoutRaw, 40, value => value === 40 ? "Ki" : `${(value + 1) * 15} s`)],
    ["14 VOICE", option(data.voiceRaw, voice)], ["15 ANI-ID", state.dtmf?.aniCode?.trim() || (state.dtmf ? "Üres" : "Nem exportált")],
    ["16 DTMFST", option(data.dtmfSidetoneRaw, ["OFF", "DT-ST", "ANI-ST", "DT+ANI"])],
    ["18 SC-REV", option(data.scanResumeRaw, ["TO", "CO", "SE"])],
    ["19 PTT-ID · globális", pttId(data.pttIdRaw)], ["20 PTT-LT · kód", data.pttDelayRaw],
    ["21 MDF-A", option(data.displayARaw, ["CH", "NAME", "FREQ"])],
    ["22 MDF-B", option(data.displayBRaw, ["CH", "NAME", "FREQ"])],
    ["23 BCL · globális", onOff(data.busyLockRaw)], ["24 AUTOLK", onOff(data.autoLockRaw)],
    ["27 MEM-CH", "Csatornamentési művelet"], ["28 DEL-CH", "Csatornatörlési művelet"],
    ["29 WT-LED", option(data.standbyLedRaw, ["Ki", "Kék", "Narancs", "Lila"])],
    ["30 RX-LED", option(data.rxLedRaw, ["Ki", "Kék", "Narancs", "Lila"])],
    ["31 TX-LED", option(data.txLedRaw, ["Ki", "Kék", "Narancs", "Lila"])],
    ["32 AL-MOD", option(data.alarmModeRaw, ["SITE", "TONE", "CODE"])],
    ["34 TDR-AB", option(data.txPriorityRaw, ["OFF", "A", "B"])],
    ["35 STE", onOff(data.tailEliminateRaw)],
    ["36 RP-STE", bounded(data.repeaterTailRaw, 10, value => value ? String(value) : "Ki")],
    ["37 RPT-RL", bounded(data.repeaterDelayRaw, 10, value => value ? `${value * 100} ms` : "Ki")],
    ["38 PONMSG", option(data.powerOnModeRaw, ["FULL", "MSG"])],
    ["39 ROGER", onOff(data.rogerRaw)], ["40 RESET", "Visszaállítási művelet"],
    ["Aktív kijelzősor", option(data.activeLineRaw, ["A", "B"])],
    ["Üzemmód", option(data.workModeRaw, ["VFO", "MR"])],
    ["A csatorna", data.channelA], ["B csatorna", data.channelB],
    ["VFO A · MHz", mhz(data.vfoAHz)], ["VFO B · MHz", mhz(data.vfoBHz)],
    ["Billentyűzár", onOff(data.keypadLockRaw)], ["FM rádió engedélyezett", onOff(data.fmRadioEnabledRaw)],
    ["Riasztás engedélyezett", onOff(data.alarmEnabledRaw)], ["RESET menü elérhető", onOff(data.resetMenuEnabledRaw)],
    ["Menühozzáférés", onOff(data.menusEnabledRaw)]
  ];
  renderDefinitions(settings, values);
  renderVfos(state);
  renderCodes(state);
  const channels = document.getElementById("channels");
  channels.replaceChildren();
  document.getElementById("count").textContent = `(${state.channels.length}/128)`;
  for (const channel of state.channels) {
    addRow(channels, [channel.number, channel.name, mhz(channel.rxHz), mhz(channel.txHz), tone(channel.rxToneRaw), tone(channel.txToneRaw), channel.bandwidth, power(channel.powerRaw), channel.scan ? "Igen" : "Nem", onOff(channel.busyLockRaw), pttId(channel.pttIdRaw), bounded(channel.signalCodeRaw, 14, value => value + 1)]);
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