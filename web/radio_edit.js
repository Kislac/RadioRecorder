const editor = document.getElementById("editor");
const editGroup = document.getElementById("editGroup");
const editChannel = document.getElementById("editChannel");
const editField = document.getElementById("editField");
const editValue = document.getElementById("editValue");
const editError = document.getElementById("editError");
const writeConfirm = document.getElementById("writeConfirm");
const writeRadio = document.getElementById("writeRadio");
let fieldSchema = [];
let editSnapshot = null;
let queuedChanges = [];
let editorRevision = "";

const fieldLabels = {squelch:"0 SQL", save:"3 SAVE", vox:"4 VOX", backlight:"6 ABR", dualWatch:"7 TDR", beep:"8 BEEP", timeoutRaw:"9 TOT", voiceRaw:"14 VOICE", dtmfSidetoneRaw:"16 DTMFST", scanResumeRaw:"18 SC-REV", pttIdRaw:"19 PTT-ID", pttDelayRaw:"20 PTT-LT", displayARaw:"21 MDF-A", displayBRaw:"22 MDF-B", busyLockRaw:"23 BCL", autoLockRaw:"24 AUTOLK", standbyLedRaw:"29 WT-LED", rxLedRaw:"30 RX-LED", txLedRaw:"31 TX-LED", alarmModeRaw:"32 AL-MOD", txPriorityRaw:"34 TDR-AB", tailEliminateRaw:"35 STE", repeaterTailRaw:"36 RP-STE", repeaterDelayRaw:"37 RPT-RL", powerOnModeRaw:"38 PONMSG", rogerRaw:"39 ROGER", frequencyHz:"Frekvencia · MHz", offsetHz:"26 OFFSET · MHz", rxHz:"RX · MHz", txHz:"TX · MHz", rxToneRaw:"RX DCS/CTCSS", txToneRaw:"TX DCS/CTCSS", shiftRaw:"25 SFT-D", signalCodeRaw:"17 S-CODE", stepRaw:"1 STEP", powerRaw:"2 TXP", bandwidthRaw:"5 W/N", name:"Csatornanév", scanRaw:"Scan", channelA:"A csatorna", channelB:"B csatorna", workModeRaw:"VFO/MR", activeLineRaw:"Aktív sor", keypadLockRaw:"Billentyűzár", fmRadioEnabledRaw:"FM rádió", aniCode:"15 ANI-ID", alarmCode:"Riasztási kód", aniModeRaw:"ANI küldés", toneOnRaw:"DTMF hangidő", toneOffRaw:"DTMF szünet"};
const choices = {voiceRaw:["Ki","Angol","Kínai"], save:["Ki","1:1","1:2","1:3","1:4"], dtmfSidetoneRaw:["OFF","DT-ST","ANI-ST","DT+ANI"], scanResumeRaw:["TO","CO","SE"], pttIdRaw:["OFF","BOT","EOT","BOTH"], aniModeRaw:["OFF","BOT","EOT","BOTH"], displayARaw:["CH","NAME","FREQ"], displayBRaw:["CH","NAME","FREQ"], shiftRaw:["OFF","+","-"], powerRaw:["HIGH","LOW"], stepRaw:["2.5 kHz","5 kHz","6.25 kHz","10 kHz","12.5 kHz","20 kHz","25 kHz","50 kHz"], alarmModeRaw:["SITE","TONE","CODE"], txPriorityRaw:["OFF","A","B"], powerOnModeRaw:["FULL","MSG"], activeLineRaw:["A","B"], workModeRaw:["VFO","MR"]};
const queuedKey = change => `${change.group}:${change.index ?? ""}:${change.field}`;

function selectedField() {
  const group = editGroup.value.startsWith("vfo") ? "vfo" : editGroup.value;
  return fieldSchema.find(field => field.group === group && field.field === editField.value);
}
function currentValue(field) {
  const source = editGroup.value === "channel" ? editSnapshot.channels.find(channel => channel.number === Number(editChannel.value)) : editSnapshot[editGroup.value];
  if (editGroup.value === "channel" && field === "bandwidthRaw") return source?.bandwidth === "Wide" ? 1 : 0;
  if (editGroup.value === "channel" && field === "scanRaw") return source?.scan ? 1 : 0;
  return source?.[field];
}
function addOption(select, value, label) {
  const option = document.createElement("option");
  option.value = value;
  option.textContent = label;
  select.append(option);
}
function renderEditValue() {
  editValue.replaceChildren();
  const field = selectedField();
  if (!field || !editSnapshot) return;
  const current = currentValue(field.field);
  let control;
  let options = choices[field.field];
  if (field.field === "bandwidthRaw") options = editGroup.value === "channel" ? ["Narrow","Wide"] : ["Wide","Narrow"];
  if (["standbyLedRaw","rxLedRaw","txLedRaw"].includes(field.field)) options = ["Ki","Kék","Narancs","Lila"];
  if (field.kind === "tone" || options) {
    control = document.createElement("select");
    if (options) options.forEach((label, value) => addOption(control, value, label));
    else {
      addOption(control, 0, "Ki");
      dtcs.forEach((code, index) => { addOption(control, index + 1, `D${String(code).padStart(3,"0")}N`); addOption(control, index + 106, `D${String(code).padStart(3,"0")}R`); });
      [670,693,719,744,770,797,825,854,885,915,948,974,1000,1035,1072,1109,1148,1188,1230,1273,1318,1365,1413,1462,1514,1567,1598,1622,1655,1679,1713,1738,1773,1799,1835,1862,1899,1928,1966,1995,2035,2065,2107,2181,2257,2291,2336,2418,2503,2541].forEach(value => addOption(control, value, `${value / 10} Hz`));
    }
    control.value = current === 65535 ? "0" : String(current);
  } else {
    control = document.createElement("input");
    control.type = field.kind === "text" ? "text" : field.max === 1 ? "checkbox" : "number";
    if (control.type === "checkbox") control.checked = current === 1;
    else if (field.kind === "text") { control.maxLength = field.max; control.value = (current || "").trim(); }
    else {
      control.min = "0";
      control.max = String(field.kind === "frequency" ? field.max / 1000000 : field.max);
      control.step = field.kind === "frequency" ? field.field === "offsetHz" ? "0.001" : "0.00001" : "1";
      control.value = current == null ? "" : String(field.kind === "frequency" ? current / 1000000 : current);
    }
  }
  control.setAttribute("aria-label", fieldLabels[field.field] || field.field);
  editValue.append(control);
  if (field.field === "txHz") {
    const disabled = document.createElement("label");
    const toggle = document.createElement("input");
    toggle.type = "checkbox";
    toggle.id = "disableChannelTx";
    toggle.checked = current === null;
    control.disabled = toggle.checked;
    toggle.addEventListener("change", () => { control.disabled = toggle.checked; });
    disabled.append(toggle, document.createTextNode("Adás tiltása"));
    editValue.append(disabled);
  }
}
function renderEditFields() {
  editField.replaceChildren();
  const group = editGroup.value.startsWith("vfo") ? "vfo" : editGroup.value;
  for (const field of fieldSchema.filter(field => field.group === group)) addOption(editField, field.field, fieldLabels[field.field] || field.field);
  document.getElementById("editChannelLabel").hidden = group !== "channel";
  renderEditValue();
}
function renderQueued() {
  const list = document.getElementById("pendingChanges");
  list.replaceChildren();
  queuedChanges.forEach((change, index) => {
    const item = document.createElement("li");
    const label = `${change.group}${change.index == null ? "" : ` ${change.index}`} · ${fieldLabels[change.field] || change.field}: ${change.value === null ? "TX OFF" : change.value}`;
    item.append(document.createTextNode(label));
    const remove = document.createElement("button");
    remove.type = "button";
    remove.textContent = "Eltávolítás";
    remove.addEventListener("click", () => { queuedChanges.splice(index, 1); writeConfirm.checked = false; renderQueued(); });
    item.append(remove);
    list.append(item);
  });
  document.getElementById("discardChanges").disabled = !queuedChanges.length;
  writeRadio.disabled = busy || !queuedChanges.length || !writeConfirm.checked || !wiring.checked;
}
function radioEditorUpdate(state) {
  const available = state.state === "done" && state.writeSupported && fieldSchema.length;
  editor.hidden = !available;
  document.getElementById("writeAvailability").textContent = state.state === "done" && !state.writeSupported ? "Az írás csak az ellenőrzött N5R2407BFB297 rádióváltozathoz engedélyezett." : "";
  if (!available) return;
  editSnapshot = state;
  if (editorRevision !== state.revision) {
    queuedChanges = [];
    editorRevision = state.revision;
    writeConfirm.checked = false;
    editChannel.replaceChildren();
    for (const channel of state.channels) addOption(editChannel, channel.number, `${channel.number} ${channel.name.trim()}`);
    renderEditFields();
  }
  renderQueued();
}
editGroup.addEventListener("change", renderEditFields);
editField.addEventListener("change", renderEditValue);
editChannel.addEventListener("change", renderEditValue);
writeConfirm.addEventListener("change", renderQueued);
wiring.addEventListener("change", renderQueued);
document.getElementById("discardChanges").addEventListener("click", () => { queuedChanges = []; writeConfirm.checked = false; renderQueued(); });
document.getElementById("editForm").addEventListener("submit", event => {
  event.preventDefault();
  const field = selectedField();
  if (!field || busy) return;
  const control = editValue.firstElementChild;
  if (!control.reportValidity()) return;
  const disabled = document.getElementById("disableChannelTx");
  let value = disabled?.checked ? null : field.kind === "text" ? control.value : control.type === "checkbox" ? Number(control.checked) : Number(control.value);
  if (field.kind === "frequency" && value !== null) value = Math.round(value * 1000000);
  const change = {group:editGroup.value, field:field.field, value};
  if (change.group === "channel") change.index = Number(editChannel.value);
  const next = queuedChanges.filter(item => queuedKey(item) !== queuedKey(change));
  next.push(change);
  if (next.length > 32 || JSON.stringify({revision:editorRevision,changes:next}).length > 4096) { editError.textContent = "Legfeljebb 32 módosítás és 4096 bájt küldhető egyszerre."; return; }
  queuedChanges = next;
  writeConfirm.checked = false;
  editError.textContent = "";
  renderQueued();
});
writeRadio.addEventListener("click", async () => {
  if (writeRadio.disabled || !confirm(`${queuedChanges.length} módosítás írása a rádióba?`)) return;
  clearTimeout(timer);
  busy = true;
  writeRadio.disabled = readRadio.disabled = true;
  try {
    render(await request("/api/radio/write?confirm=WRITE%20UV5R", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify({revision:editorRevision,changes:queuedChanges})}));
  } catch (error) { editError.textContent = error.message; busy = false; renderQueued(); }
  timer = setTimeout(poll, 500);
});
request("/api/radio/schema").then(schema => {
  if (!Array.isArray(schema.fields)) throw new Error("Érvénytelen mezőséma");
  fieldSchema = schema.fields;
  if (snapshot) radioEditorUpdate(snapshot);
}).catch(error => { editError.textContent = error.message; });