const pttOn = document.querySelector("#pttOn"),
  pttOff = document.querySelector("#pttOff"),
  pttStatus = document.querySelector("#pttStatus");
async function refreshPtt() {
  try {
    const response = await fetch("/api/ptt-test", { cache: "no-store" });
    if (!response.ok) throw Error();
    const state = await response.json();
    pttOn.disabled = state.active;
    pttStatus.textContent = state.active ? "PTT aktív" : "PTT kikapcsolva";
  } catch {
    pttOn.disabled = true;
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