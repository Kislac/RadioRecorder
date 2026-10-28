const pttOn = document.querySelector("#pttOn"),
  pttToggle = document.querySelector("#pttToggle"),
  pttOff = document.querySelector("#pttOff"),
  pttStatus = document.querySelector("#pttStatus");
let pttActive = false;
async function refreshPtt() {
  try {
    const response = await fetch("/api/ptt-test", { cache: "no-store" });
    if (!response.ok) throw Error();
    const state = await response.json();
    pttActive = state.active;
    pttOn.disabled = state.active;
    pttToggle.disabled = false;
    pttToggle.setAttribute("aria-pressed", String(state.active));
    pttToggle.textContent = state.active ? "Relé kikapcsolása" : "Relé bekapcsolása";
    pttStatus.textContent = state.active ? "PTT aktív" : "PTT kikapcsolva";
  } catch {
    pttOn.disabled = true;
    pttToggle.disabled = true;
    pttStatus.textContent = "PTT állapot nem elérhető";
  }
}
pttOn.addEventListener("click", async () => {
  pttOn.disabled = true;
  try {
    const response = await fetch("/api/ptt-test/on", { method: "POST" });
    if (!response.ok) throw Error(await response.text());
  } catch {
    pttStatus.textContent = "PTT teszt nem indult el";
  }
  await refreshPtt();
});
pttToggle.addEventListener("click", async () => {
  pttToggle.disabled = true;
  try {
    const response = await fetch(pttActive ? "/api/ptt-test/off" : "/api/ptt-test/on", { method: "POST" });
    if (!response.ok) throw Error();
    await refreshPtt();
  } catch {
    pttStatus.textContent = "A relé állapota nem igazolható; ellenőrizd a relét!";
    await refreshPtt();
  }
});
pttOff.addEventListener("click", async () => {
  pttOff.disabled = true;
  try {
    const response = await fetch("/api/ptt-test/off", { method: "POST" });
    if (!response.ok) throw Error();
    await refreshPtt();
  } catch {
    pttStatus.textContent = "Kikapcsolás nem igazolható; ellenőrizd a relét!";
  } finally {
    pttOff.disabled = false;
  }
});
refreshPtt();
setInterval(refreshPtt, 500);