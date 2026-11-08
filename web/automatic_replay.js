const replay = id => document.getElementById(id);
let replayFolder = "/", replaySelected = new Set(), replaySelectedFolders = new Set(), replayRules = [], replaySnapshot = {};

async function replayRequest(url, body) {
  const response = await fetch(url, body === undefined ? {cache:"no-store"} :
    {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)});
  if (!response.ok) throw Error(await response.text());
  return response.json();
}

function replayMessage(error) { replay("replayError").textContent = error?.message || String(error || ""); }

async function replayRefresh() {
  try {
    replaySnapshot = await replayRequest("/api/automatic-replay");
    replayRules = replaySnapshot.rules || [];
    replay("replayState").textContent = replaySnapshot.armed ? "Armed" : replaySnapshot.armPending ? "Awaiting local confirmation" : "Disarmed";
    replay("replayQueue").textContent = (replaySnapshot.queueLength || 0) + " waiting";
    replay("replayArm").disabled = !replaySnapshot.storageValid || !replayRules.some(rule => rule.active) || replaySnapshot.armed;
    if (replaySnapshot.error) replayMessage(replaySnapshot.error);
    const list = replay("replayRules"); list.replaceChildren();
    for (const rule of replayRules) {
      const item = document.createElement("div"); item.className = "replay-rule";
      const title = document.createElement("strong"); title.textContent = rule.path;
      const detail = document.createElement("p");
      detail.textContent = `#${rule.id} · ${rule.completed ? "Completed" : rule.active ? "Active" : "Inactive"} · ${rule.folder ? `Folder (${rule.fileCount || 0} WAVs, ${rule.cursor || "start"}) · ` : ""}${rule.intervalSeconds ? "Every " + rule.intervalSeconds / 60 + " min" : "Once"} · Next: ${rule.nextUtc ? new Date(rule.nextUtc * 1000).toLocaleString() : "—"}`;
      const toggle = document.createElement("button"); toggle.type = "button";
      toggle.textContent = rule.active ? "Deactivate" : "Activate";
      toggle.disabled = replaySnapshot.armed || rule.completed;
      toggle.addEventListener("click", async () => {
        try { await replayRequest(`/api/automatic-replay/activate?id=${rule.id}&enabled=${rule.active ? 0 : 1}`, {}); replayMessage(""); await replayRefresh(); }
        catch (error) { replayMessage(error); }
      });
      const edit = document.createElement("button"); edit.type = "button"; edit.textContent = "Edit";
      edit.disabled = rule.active || replaySnapshot.armed;
      edit.addEventListener("click", () => {
        replaySelected.clear(); replaySelectedFolders.clear(); replay("replayPath").value = rule.path;
        replay("replayForm").dataset.id = rule.id;
        replay("replayIsFolder").checked = Boolean(rule.folder);
        replay("replayIsFolder").dispatchEvent(new Event("change"));
        replay("replayOrder").value = rule.order || 0;
        replay("replayCycle").value = rule.cycle || 0;
        replay("replayBatch").value = rule.batch || 0;
        replay("replayMode").value = rule.intervalSeconds ? "regular" : "once";
        replay("replayMode").dispatchEvent(new Event("change"));
        replay("replayStartMode").value = "date";
        replay("replayStartMode").dispatchEvent(new Event("change"));
        replay("replayDate").value = new Date(rule.anchorUtc * 1000).toLocaleString("sv-SE").replace(" ", "T").slice(0, 16);
        replay("replayHours").value = Math.floor(rule.intervalSeconds / 3600);
        replay("replayMinutes").value = Math.floor(rule.intervalSeconds % 3600 / 60);
        replay("replayJitter").value = rule.jitterMinutes;
        replay("replayWindow").value = rule.window;
        replay("replayWindow").dispatchEvent(new Event("change"));
        replay("replayFrom").value = `${String(Math.floor(rule.startMinute / 60)).padStart(2,"0")}:${String(rule.startMinute % 60).padStart(2,"0")}`;
        replay("replayTo").value = `${String(Math.floor(rule.endMinute / 60)).padStart(2,"0")}:${String(rule.endMinute % 60).padStart(2,"0")}`;
        replay("replayForm").scrollIntoView({behavior:"smooth"});
      });
      item.append(title, detail, toggle, edit); list.append(item);
    }
  } catch (error) { replayMessage(error); }
}

async function replayBrowse(folder = replayFolder) {
  try {
    replayFolder = folder; replay("replayFolder").textContent = folder === "/" ? "microSD" : folder;
    const list = replay("replayFiles"); list.replaceChildren();
    let after = "", revision = "";
    do {
      const params = new URLSearchParams({dir:folder, sort:"name", order:"asc"});
      if (after) { params.set("after", after); params.set("revision", revision); }
      const page = await replayRequest("/api/files?" + params);
      for (const entry of page.items) {
        if (entry.directory) {
          const button = document.createElement("button"); button.type = "button";
          button.textContent = entry.name + "/";
          button.addEventListener("click", () => replayBrowse(entry.path));
          const label = document.createElement("label"), check = document.createElement("input");
          check.type = "checkbox"; check.checked = replaySelectedFolders.has(entry.path);
          check.addEventListener("change", () => {
            if (check.checked) { replaySelected.add(entry.path); replaySelectedFolders.add(entry.path); replay("replayPath").value = entry.path; replay("replayIsFolder").checked = true; }
            else { replaySelected.delete(entry.path); replaySelectedFolders.delete(entry.path); }
            replay("replayIsFolder").dispatchEvent(new Event("change"));
            replay("replaySelection").textContent = replaySelected.size + " selected";
          });
          label.append(check, button); list.append(label);
        } else if (/\.wav$/i.test(entry.name)) {
          const label = document.createElement("label"), check = document.createElement("input");
          check.type = "checkbox"; check.checked = replaySelected.has(entry.path);
          check.addEventListener("change", () => {
            if (check.checked) { replaySelected.add(entry.path); replay("replayPath").value = entry.path; replay("replayIsFolder").checked = false; }
            else replaySelected.delete(entry.path);
            replay("replayIsFolder").dispatchEvent(new Event("change"));
            replay("replaySelection").textContent = replaySelected.size + " selected";
          });
          label.append(check, document.createTextNode(entry.name)); list.append(label);
        }
      }
      after = page.next || ""; revision = page.revision;
    } while (after);
  } catch (error) { replayMessage(error); }
}

const replayMinute = value => { const [hours, minutes] = value.split(":").map(Number); return hours * 60 + minutes; };
replay("replayIsFolder").addEventListener("change", () => { replay("replayFolderOptions").hidden = !replay("replayIsFolder").checked; });
function replayDelaySeconds() {
  const mode = replay("replayDelayMode").value;
  const amount = mode === "parts" ?
    ((Number(replay("replayDays").value) * 24 + Number(replay("replayDelayHours").value)) * 60 + Number(replay("replayDelayMinutes").value)) * 60 + Number(replay("replayDelaySeconds").value) :
    Number(replay("replayDelayTotal").value) * ({seconds:1, minutes:60, hours:3600}[mode] || 0);
  if (!Number.isSafeInteger(amount) || amount < 1 || amount > 31536000) throw Error("Delay must be 1 second to 365 days");
  return amount;
}
replay("replayMode").addEventListener("change", () => {
  const regular = replay("replayMode").value === "regular";
  replay("replayInterval").hidden = !regular;
  if (regular && replay("replayStartMode").value === "delay") {
    replay("replayStartMode").value = "date";
    replay("replayStartMode").dispatchEvent(new Event("change"));
  }
});
replay("replayStartMode").addEventListener("change", () => {
  const delayed = replay("replayStartMode").value === "delay";
  replay("replayDateField").hidden = delayed;
  replay("replayDate").required = !delayed;
  replay("replayDelay").hidden = !delayed;
  if (delayed) { replay("replayMode").value = "once"; replay("replayMode").dispatchEvent(new Event("change")); }
});
replay("replayDelayMode").addEventListener("change", () => {
  const parts = replay("replayDelayMode").value === "parts";
  replay("replayDelayParts").hidden = !parts;
  replay("replayDelayTotalField").hidden = parts;
});
replay("replayWindow").addEventListener("change", () => { replay("replayWindowTimes").hidden = replay("replayWindow").value === "0"; });
replay("replayForm").addEventListener("submit", async event => {
  event.preventDefault();
  const intervalSeconds = replay("replayMode").value === "regular" ?
    (Number(replay("replayHours").value) * 60 + Number(replay("replayMinutes").value)) * 60 : 0;
  const anchorUtc = Math.floor(new Date(replay("replayDate").value).getTime() / 1000);
  const paths = replaySelected.size ? [...replaySelected] : [replay("replayPath").value];
  const id = Number(replay("replayForm").dataset.id);
  try {
    const delayed = replay("replayStartMode").value === "delay";
    if ((!delayed && !Number.isSafeInteger(anchorUtc)) || (intervalSeconds && intervalSeconds <= Number(replay("replayJitter").value) * 120)) throw Error("Invalid schedule");
    const start = delayed ? {delaySeconds:replayDelaySeconds()} : {anchorUtc};
    for (const path of paths) {
      const folder = replaySelected.size ? replaySelectedFolders.has(path) : replay("replayIsFolder").checked;
      await replayRequest("/api/automatic-replay/rules", {
      ...(id ? {id} : {}), path, ...start, intervalSeconds, ...(folder ? {folder:true,
        order:Number(replay("replayOrder").value), cycle:Number(replay("replayCycle").value), batch:Number(replay("replayBatch").value)} : {}),
      jitterMinutes:intervalSeconds ? Number(replay("replayJitter").value) : 0,
      window:Number(replay("replayWindow").value),
      startMinute:replayMinute(replay("replayFrom").value), endMinute:replayMinute(replay("replayTo").value),
      });
    }
    delete replay("replayForm").dataset.id; replaySelected.clear(); replaySelectedFolders.clear();
    replayMessage(""); replay("replaySelection").textContent = "";
    await replayRefresh(); await replayBrowse();
  } catch (error) { replayMessage(error); }
});
replay("replayParent").addEventListener("click", () => replayBrowse(replayFolder.slice(0, replayFolder.lastIndexOf("/")) || "/"));
replay("replayRefresh").addEventListener("click", () => replayBrowse());
replay("replayStop").addEventListener("click", async () => {
  try { await replayRequest("/api/automatic-replay/stop", {}); replayMessage(""); await replayRefresh(); }
  catch (error) { replayMessage(error); }
});
replay("replayArm").addEventListener("click", async () => {
  if (!confirm("Only proceed after measuring radio TOT and verifying PTT wiring and legal test conditions. Request local confirmation?")) return;
  try { await replayRequest("/api/automatic-replay/arm?confirm=MEASURED%20TOT", {}); replayMessage("Confirm on the encoder within 30 seconds"); await replayRefresh(); }
  catch (error) { replayMessage(error); }
});
replay("replayDate").value = new Date(Date.now() + 3600000).toLocaleString("sv-SE").replace(" ", "T").slice(0,16);
const replayParams = new URLSearchParams(location.search);
for (const path of replayParams.getAll("path")) if (path.startsWith("/") && /\.wav$/i.test(path)) replaySelected.add(path);
for (const path of replayParams.getAll("folder")) if (path.startsWith("/")) { replaySelected.add(path); replaySelectedFolders.add(path); }
if (replaySelected.size) {
  replay("replayPath").value = [...replaySelected][0]; replay("replayIsFolder").checked = replaySelectedFolders.has(replay("replayPath").value);
  replay("replayIsFolder").dispatchEvent(new Event("change"));
  replay("replaySelection").textContent = replaySelected.size + " selected";
}
replayRefresh(); replayBrowse(); setInterval(replayRefresh, 2000);