const replay = id => document.getElementById(id);
let replayFolder = "/", replaySelected = new Set(), replayRules = [], replaySnapshot = {};

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
      detail.textContent = `#${rule.id} · ${rule.completed ? "Completed" : rule.active ? "Active" : "Inactive"} · ${rule.intervalSeconds ? "Every " + rule.intervalSeconds / 60 + " min" : "Once"} · Next: ${rule.nextUtc ? new Date(rule.nextUtc * 1000).toLocaleString() : "—"}`;
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
        replaySelected.clear(); replay("replayPath").value = rule.path;
        replay("replayForm").dataset.id = rule.id;
        replay("replayMode").value = rule.intervalSeconds ? "regular" : "once";
        replay("replayMode").dispatchEvent(new Event("change"));
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
          button.addEventListener("click", () => replayBrowse(entry.path)); list.append(button);
        } else if (/\.wav$/i.test(entry.name)) {
          const label = document.createElement("label"), check = document.createElement("input");
          check.type = "checkbox"; check.checked = replaySelected.has(entry.path);
          check.addEventListener("change", () => {
            if (check.checked) { replaySelected.add(entry.path); replay("replayPath").value = entry.path; }
            else replaySelected.delete(entry.path);
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
replay("replayMode").addEventListener("change", () => { replay("replayInterval").hidden = replay("replayMode").value !== "regular"; });
replay("replayWindow").addEventListener("change", () => { replay("replayWindowTimes").hidden = replay("replayWindow").value === "0"; });
replay("replayForm").addEventListener("submit", async event => {
  event.preventDefault();
  const intervalSeconds = replay("replayMode").value === "regular" ?
    (Number(replay("replayHours").value) * 60 + Number(replay("replayMinutes").value)) * 60 : 0;
  const anchorUtc = Math.floor(new Date(replay("replayDate").value).getTime() / 1000);
  const paths = replaySelected.size ? [...replaySelected] : [replay("replayPath").value];
  const id = Number(replay("replayForm").dataset.id);
  try {
    if (!Number.isSafeInteger(anchorUtc) || (intervalSeconds && intervalSeconds <= Number(replay("replayJitter").value) * 120)) throw Error("Invalid schedule");
    for (const path of paths) await replayRequest("/api/automatic-replay/rules", {
      ...(id ? {id} : {}), path, anchorUtc, intervalSeconds,
      jitterMinutes:intervalSeconds ? Number(replay("replayJitter").value) : 0,
      window:Number(replay("replayWindow").value),
      startMinute:replayMinute(replay("replayFrom").value), endMinute:replayMinute(replay("replayTo").value),
    });
    delete replay("replayForm").dataset.id; replaySelected.clear();
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
for (const path of new URLSearchParams(location.search).getAll("path")) if (path.startsWith("/") && /\.wav$/i.test(path)) replaySelected.add(path);
if (replaySelected.size) { replay("replayPath").value = [...replaySelected][0]; replay("replaySelection").textContent = replaySelected.size + " selected"; }
replayRefresh(); replayBrowse(); setInterval(replayRefresh, 2000);