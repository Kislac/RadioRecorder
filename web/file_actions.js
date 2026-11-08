const selectedFiles = new Map();
let fileRevision = "", operationActive = false, stopQueue = false, currentJob = 0, uploadRequest = null, queuedReload = false, uploadFailure = "";
let dialogAction = "", dialogItems = [], destinationFolder = folder, searchTimer = null;
const fileDialog = document.querySelector("#fileDialog"), entryName = document.querySelector("#entryName");
const operationPanel = document.querySelector("#fileOperation"), operationStatus = document.querySelector("#operationStatus");
const formatBytes = value => value >= 1024**3 ? (value / 1024**3).toFixed(2) + " GiB" : value >= 1024**2 ? (value / 1024**2).toFixed(1) + " MiB" : value >= 1024 ? (value / 1024).toFixed(1) + " KiB" : Math.round(value) + " B";
const childPath = (parent, name) => (parent === "/" ? "" : parent) + "/" + name;

async function fileRequest(path, body) {
  const response = await fetch(path, body === undefined ? {cache:"no-store"} : {
    method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body),
  });
  if (!response.ok) throw Error(await response.text());
  return response.json();
}

function updateSelection() {
  document.querySelector("#selectionCount").textContent = selectedFiles.size + " selected";
  for (const id of ["copySelected", "moveSelected", "deleteSelected"]) document.querySelector("#" + id).disabled = !selectedFiles.size || operationActive;
  document.querySelector("#replaySelected").disabled = operationActive || ![...selectedFiles.values()].some(item => item.directory || /\.wav$/i.test(item.name));
  const checks = [...listEl.querySelectorAll('input[type="checkbox"]')];
  document.querySelector("#selectAll").checked = checks.length > 0 && checks.every(check => check.checked);
  document.querySelector("#selectAll").indeterminate = checks.some(check => check.checked) && !checks.every(check => check.checked);
}

function actionButton(label, action) {
  const button = document.createElement("button");
  button.type = "button"; button.textContent = label; button.title = label;
  button.disabled = operationActive; button.addEventListener("click", action); return button;
}

function render(item) {
  const row = document.createElement("tr"), selection = document.createElement("td"), check = document.createElement("input");
  check.type = "checkbox"; check.setAttribute("aria-label", "Select " + item.name);
  check.addEventListener("change", () => { if (check.checked) selectedFiles.set(item.path, item); else selectedFiles.delete(item.path); updateSelection(); });
  selection.append(check); row.append(selection);
  const name = document.createElement("td"); name.className = "name-cell";
  const label = document.createElement(item.directory ? "a" : "span");
  label.textContent = item.name; label.dataset.i18nIgnore = "";
  if (item.directory) label.href = link(item.path);
  name.append(label); row.append(name);
  const date = document.createElement("td"); date.className = "date-column";
  date.textContent = item.modified > 946684800 ? new Date(item.modified * 1000).toISOString().replace("T", " ").slice(0,19) + " UTC" : "Unknown";
  const size = document.createElement("td"); size.textContent = item.directory ? "Folder" : formatBytes(item.bytes);
  row.append(date, size);
  const actions = document.createElement("td"), container = document.createElement("div"); container.className = "row-actions";
  const menu = document.createElement("details"), summary = document.createElement("summary"), options = document.createElement("div");
  menu.className = "row-menu"; summary.textContent = "☰"; summary.title = "File actions";
  summary.setAttribute("aria-label", "File actions"); menu.append(summary);
  if (item.directory) options.append(actionButton("Download as ZIP", () => runQueue("zip", [item])));
  if (item.directory) {
    const replayLink = document.createElement("a");
    replayLink.href = "/automatic-replay?folder=" + encodeURIComponent(item.path);
    replayLink.textContent = "Automatic replay"; options.append(replayLink);
  }
  else {
    const download = document.createElement("a"); download.href = "/file?path=" + encodeURIComponent(item.path) + "&download=1";
    download.textContent = "Download"; download.download = item.name; options.append(download);
    download.addEventListener("click", event => { if (operationActive) { event.preventDefault(); message.textContent = "File operation busy"; } });
    if (sound(item.name)) {
      const browserButton = actionButton("Play in browser", () => openPlayback(item, "browser"));
      browserButton.dataset.browserPlay = "";
      options.append(browserButton);
    }
  }
  options.append(actionButton("Rename", () => openDialog("rename", [item])),
    actionButton("Copy", () => openDialog("copy", [item])), actionButton("Move", () => openDialog("move", [item])),
    actionButton("Delete", () => deleteItems([item])));
  if (!item.directory && /\.wav$/i.test(item.name)) {
    const speaker = actionButton("Play on Speaker", () => openPlayback(item, "speaker"));
    const radio = actionButton("Play on Radio", () => openPlayback(item, "radio"));
    speaker.dataset.espPlay = ""; radio.dataset.radioPlay = "";
    options.append(speaker, radio);
    const replayLink = document.createElement("a");
    replayLink.href = "/automatic-replay?path=" + encodeURIComponent(item.path);
    replayLink.textContent = "Automatic replay"; options.append(replayLink);
  }
  menu.addEventListener("toggle", () => {
    if (!menu.open) return;
    for (const other of document.querySelectorAll(".row-menu[open]")) if (other !== menu) other.open = false;
    const anchor = summary.getBoundingClientRect();
    const bottomNav = document.querySelector(".bottom-nav");
    const bottom = bottomNav && getComputedStyle(bottomNav).display !== "none" ? bottomNav.getBoundingClientRect().top : innerHeight;
    options.style.left = Math.max(8, Math.min(anchor.right - options.offsetWidth, innerWidth - options.offsetWidth - 8)) + "px";
    options.style.top = (anchor.bottom + options.offsetHeight + 8 <= bottom
      ? anchor.bottom : Math.max(8, Math.min(anchor.top - options.offsetHeight, bottom - options.offsetHeight - 8))) + "px";
  });
  options.addEventListener("click", event => { if (event.target.closest("a,button")) menu.open = false; });
  menu.append(options); container.append(menu); actions.append(container); row.append(actions); listEl.append(row);
}

document.addEventListener("pointerdown", event => {
  for (const menu of document.querySelectorAll(".row-menu[open]")) if (!menu.contains(event.target)) menu.open = false;
});
addEventListener("scroll", event => {
  if (event.target.closest?.(".row-menu")) return;
  for (const menu of document.querySelectorAll(".row-menu[open]")) menu.open = false;
}, true);
addEventListener("resize", () => {
  for (const menu of document.querySelectorAll(".row-menu[open]")) menu.open = false;
});

async function load(reset = false) {
  if (loading) { queuedReload = queuedReload || reset; return; }
  if (!reset && next === null) return;
  loading = true; loadFailed = false;
  if (reset) { listEl.replaceChildren(); selectedFiles.clear(); next = ""; fileRevision = ""; updateSelection(); }
  try {
    const params = new URLSearchParams({dir:folder, sort:document.querySelector("#fileSort").value,
      order:document.querySelector("#fileOrder").value, filter:document.querySelector("#fileSearch").value});
    if (next) params.set("after", next);
    if (fileRevision) params.set("revision", fileRevision);
    const data = await fileRequest("/api/files?" + params);
    data.items.forEach(render); next = data.next; fileRevision = data.revision;
    document.querySelector("#storage").textContent = formatBytes(data.capacity - data.used) + " free / " + formatBytes(data.capacity) + " total";
    message.textContent = data.total ? data.total + " items" : "Empty folder.";
    more.hidden = next === null; more.textContent = "More";
    refreshSpeaker();
  } catch (error) {
    message.textContent = error.message; loadFailed = true; more.hidden = false; more.textContent = "Retry";
  } finally { loading = false; }
  if (queuedReload) { queuedReload = false; await load(true); return; }
  if (!loadFailed && next !== null) requestAnimationFrame(() => {
    if (!loading && more.getBoundingClientRect().top <= innerHeight + 240) load();
  });
}

async function browseDestination(path) {
  destinationFolder = path; document.querySelector("#destinationPath").textContent = path;
  const container = document.querySelector("#destinationFolders"); container.replaceChildren();
  let after = "", revision = "", moreFolders = true;
  while (moreFolders) {
    const data = await fileRequest("/api/files?" + new URLSearchParams({dir:path, sort:"name", order:"asc", after, revision}));
    for (const item of data.items) {
      if (!item.directory) { moreFolders = false; break; }
      const button = actionButton(item.name, () => browseDestination(item.path).catch(showDialogError));
      button.dataset.i18nIgnore = ""; container.append(button);
    }
    after = data.next; revision = data.revision; if (!after) break;
  }
  document.querySelector("#destinationParent").disabled = path === "/";
}

function showDialogError(error) { document.querySelector("#dialogError").textContent = error.message; }
function openDialog(action, items = []) {
  if (operationActive) return;
  dialogAction = action; dialogItems = items;
  document.querySelector("#dialogTitle").textContent = ({rename:"Rename", mkdir:"New folder", copy:"Copy", move:"Move"})[action];
  document.querySelector("#dialogError").textContent = "";
  const needsName = action === "rename" || action === "mkdir" || items.length === 1;
  document.querySelector("#nameLabel").hidden = !needsName;
  document.querySelector("#destinationPicker").hidden = action !== "copy" && action !== "move";
  entryName.value = items.length === 1 ? items[0].name : "";
  entryName.required = needsName;
  fileDialog.showModal();
  if (action === "copy" || action === "move") browseDestination(folder).catch(showDialogError);
  else { entryName.focus(); entryName.select(); }
}

document.querySelector("#fileDialogForm").addEventListener("submit", async event => {
  event.preventDefault();
  document.querySelector("#dialogSubmit").disabled = true;
  try {
    if (dialogAction === "mkdir" || dialogAction === "rename") {
      if (!entryName.value || /[\\/]/.test(entryName.value)) throw Error("Invalid name");
      await fileRequest("/api/files/" + (dialogAction === "mkdir" ? "mkdir" : "rename"), dialogAction === "mkdir" ?
        {path:childPath(folder, entryName.value)} : {source:dialogItems[0].path, destination:childPath(folder, entryName.value)});
      fileDialog.close(); await load(true);
    } else {
      if (dialogItems.length === 1 && (!entryName.value || /[\\/]/.test(entryName.value))) throw Error("Invalid name");
      const destinationName = dialogItems.length === 1 ? entryName.value : "";
      fileDialog.close(); await runQueue(dialogAction, dialogItems, destinationFolder, destinationName);
    }
  } catch (error) { showDialogError(error); }
  finally { document.querySelector("#dialogSubmit").disabled = false; }
});
document.querySelector("#dialogCancel").addEventListener("click", () => fileDialog.close());
document.querySelector("#destinationParent").addEventListener("click", () => browseDestination(destinationFolder.slice(0, destinationFolder.lastIndexOf('/')) || "/").catch(showDialogError));

async function deleteItems(items) {
  if (!items.length) return;
  const sourceQuestion = "Delete " + items.length + " selected items and their contents? This cannot be undone.";
  const question = window.RR_I18N ? window.RR_I18N.translate(sourceQuestion) : sourceQuestion;
  if (!confirm(question + "\n\n" + items.map(item => item.path).join("\n"))) return;
  await runQueue("delete", items);
}

function transferUrl(job, kind) {
  return job.transferPort ? "http://" + location.hostname + ":" + job.transferPort + "/" + kind + "?id=" + job.id : "/transfer/" + kind + "?id=" + job.id;
}

function beginUpload(job, file) {
  uploadRequest = new XMLHttpRequest(); uploadRequest.open("PUT", transferUrl(job, "upload"));
  uploadRequest.setRequestHeader("Content-Type", "application/octet-stream");
  uploadRequest.upload.addEventListener("progress", event => {
    if (event.lengthComputable) document.querySelector("#operationCounters").textContent = formatBytes(event.loaded) + " / " + formatBytes(event.total);
  });
  uploadRequest.addEventListener("error", () => { uploadFailure = "Upload connection failed"; });
  uploadRequest.addEventListener("load", () => { if (uploadRequest.status < 200 || uploadRequest.status >= 300) uploadFailure = "Upload failed"; });
  uploadRequest.send(file);
}

async function monitorJob(initial, uploadFile) {
  currentJob = initial.id;
  document.querySelector("#cancelOperation").disabled = false;
  let attached = false, start = performance.now(), job = initial;
  while (true) {
    if (uploadFailure) {
      await fileRequest("/api/files/jobs/cancel?id=" + currentJob, {}).catch(() => {});
      throw Error(uploadFailure);
    }
    document.querySelector("#operationTitle").textContent = ({copy:"Copy", move:"Move", delete:"Delete", zip:"ZIP download", upload:"Upload"})[job.action] || "Operation";
    const progress = document.querySelector("#operationProgress");
    const transferred = job.action === "zip" ? job.sentBytes : job.completedBytes;
    const total = job.action === "zip" ? job.archiveBytes : job.totalBytes;
    if (total && !["preparing", "ready", "verifying"].includes(job.state)) progress.value = Math.min(100, transferred / total * 100);
    else progress.removeAttribute("value");
    operationStatus.textContent = ({preparing:"Preparing", ready:"Waiting for transfer", running:"In progress", verifying:"Verifying", complete:"Complete", cancelled:"Cancelled", failed:"Failed"})[job.state] || job.state;
    const seconds = Math.max(0.001, (performance.now() - start) / 1000), speed = transferred / seconds;
    const remaining = speed > 0 && total >= transferred ? Math.ceil((total - transferred) / speed) : 0;
    document.querySelector("#operationCounters").textContent = job.completedFiles + " / " + job.totalFiles + " files · " +
      formatBytes(transferred) + " / " + formatBytes(total) + (speed && job.state !== "verifying" ? " · " + formatBytes(speed) + "/s · " + remaining + " s remaining" : "");
    if (job.state === "preparing") document.querySelector("#operationCounters").textContent = job.totalFiles + " files found · " + formatBytes(job.totalBytes);
    if (job.state === "ready" && !attached) {
      if (job.action === "zip") {
        const link = document.querySelector("#zipDownload"); link.hidden = false; link.href = transferUrl(job, "zip");
        document.querySelector("#downloadFrame").src = link.href;
      } else if (job.action === "upload" && uploadFile) beginUpload(job, uploadFile);
      attached = true;
    }
    if (job.action === "zip" && job.state !== "ready") document.querySelector("#zipDownload").hidden = true;
    if (["complete", "failed", "cancelled"].includes(job.state)) {
      document.querySelector("#zipDownload").hidden = true;
      document.querySelector("#cancelOperation").disabled = true;
      currentJob = 0;
      if (job.state !== "complete") throw Error(job.error || operationStatus.textContent);
      progress.value = 100; return;
    }
    await new Promise(resolve => setTimeout(resolve, 300));
    job = await fileRequest("/api/files/jobs/status?id=" + currentJob);
  }
}

async function runQueue(action, items, destination, destinationName = "") {
  if (operationActive || !items.length) return;
  operationActive = true; stopQueue = false; uploadFailure = ""; operationPanel.hidden = false;
  document.querySelector("#cancelOperation").disabled = false; updateSelection();
  try {
    let index = 0;
    for (const item of items) {
      if (stopQueue) break;
      document.querySelector("#operationQueue").textContent = (++index) + " / " + items.length + " items";
      const body = {action, source:item.path || "", confirm:action === "delete"};
      if (action === "copy" || action === "move") body.destination = childPath(destination, items.length === 1 && destinationName ? destinationName : item.name);
      if (action === "upload") { body.destination = childPath(folder, item.name); body.bytes = item.size; }
      const job = await fileRequest("/api/files/jobs", body);
      await monitorJob(job, action === "upload" ? item : null);
    }
  } catch (error) { operationStatus.textContent = error.message; }
  finally { operationActive = false; uploadRequest = null; document.querySelector("#cancelOperation").disabled = true; await load(true); }
}

document.querySelector("#cancelOperation").addEventListener("click", async () => {
  stopQueue = true; if (uploadRequest) uploadRequest.abort();
  if (currentJob) try { await fileRequest("/api/files/jobs/cancel?id=" + currentJob, {}); } catch (error) { operationStatus.textContent = error.message; }
});
document.querySelector("#selectAll").addEventListener("change", event => {
  const checked = event.target.checked;
  for (const check of listEl.querySelectorAll('input[type="checkbox"]')) { check.checked = checked; check.dispatchEvent(new Event("change")); }
});
for (const action of ["copy", "move"]) document.querySelector("#" + action + "Selected").addEventListener("click", () => openDialog(action, [...selectedFiles.values()]));
document.querySelector("#replaySelected").addEventListener("click", () => {
  const paths = new URLSearchParams();
  for (const item of selectedFiles.values()) {
    if (item.directory) paths.append("folder", item.path);
    else if (/\.wav$/i.test(item.name)) paths.append("path", item.path);
  }
  location.href = "/automatic-replay?" + paths;
});
document.querySelector("#deleteSelected").addEventListener("click", () => deleteItems([...selectedFiles.values()]));
document.querySelector("#newFolder").addEventListener("click", () => openDialog("mkdir"));
document.querySelector("#uploadButton").addEventListener("click", () => document.querySelector("#uploadFiles").click());
document.querySelector("#uploadFiles").addEventListener("change", event => { runQueue("upload", [...event.target.files]); event.target.value = ""; });
document.querySelector("#reload").addEventListener("click", () => load(true));
for (const id of ["fileSort", "fileOrder"]) document.querySelector("#" + id).addEventListener("change", () => load(true));
document.querySelector("#fileSearch").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(() => load(true), 250); });
more.addEventListener("click", () => load(loadFailed));
if ("IntersectionObserver" in window) new IntersectionObserver(entries => {
  if (entries[0].isIntersecting && !loading && !loadFailed && next !== null) load();
}, {rootMargin:"0px 0px 240px 0px"}).observe(more);
breadcrumbs(); load(true);
fileRequest("/api/files/jobs/status").then(job => {
  if (["preparing", "ready", "running", "verifying"].includes(job.state)) {
    operationPanel.hidden = false; operationActive = true;
    monitorJob(job).catch(error => { operationStatus.textContent = error.message; }).finally(() => { operationActive = false; load(true); });
  }
}).catch(() => {});