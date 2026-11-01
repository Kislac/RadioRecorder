const pttOn = document.querySelector("#pttOn"),
  pttToggle = document.querySelector("#pttToggle"),
  pttOff = document.querySelector("#pttOff"),
  pttStatus = document.querySelector("#pttStatus");
const adcPin = document.querySelector("#adcPin"),
  adcScale = document.querySelector("#adcScale"),
  adcToggle = document.querySelector("#adcToggle"),
  adcChart = document.querySelector("#adcChart"),
  adcStatus = document.querySelector("#adcStatus"),
  adcStats = document.querySelector("#adcStats");
let adcRunning = false;
let adcTimer;
let adcSamples = [];
let adcGeneration = 0;

function drawAdc() {
  const width = adcChart.clientWidth;
  const height = adcChart.clientHeight;
  const scale = Math.min(window.devicePixelRatio || 1, 2);
  adcChart.width = Math.round(width * scale);
  adcChart.height = Math.round(height * scale);
  const context = adcChart.getContext("2d");
  context.scale(scale, scale);
  const left = 48, right = 12, top = 14, bottom = 22;
  const minimum = adcSamples.length ? Math.min(...adcSamples) : 0;
  const maximum = adcSamples.length ? Math.max(...adcSamples) : 4095;
  const margin = Math.max(16, (maximum - minimum) * 0.2);
  const low = adcScale.value === "auto" && adcSamples.length ? Math.max(0, Math.floor(minimum - margin)) : 0;
  const high = adcScale.value === "auto" && adcSamples.length ? Math.min(4095, Math.ceil(maximum + margin)) : 4095;
  context.font = "12px Georgia";
  context.textAlign = "right";
  context.fillStyle = "#52635a";
  context.strokeStyle = "#e2e8e3";
  for (let step = 0; step <= 4; step++) {
    const value = Math.round(high - (high - low) * step / 4);
    const y = top + (height - top - bottom) * step / 4;
    context.fillText(String(value), left - 7, y + 4);
    context.beginPath();
    context.moveTo(left, y);
    context.lineTo(width - right, y);
    context.stroke();
  }
  if (!adcSamples.length) return;
  context.strokeStyle = "#187465";
  context.lineWidth = 2;
  context.beginPath();
  adcSamples.forEach((value, index) => {
    const x = left + index * (width - left - right) / (adcSamples.length - 1);
    const y = top + (high - value) * (height - top - bottom) / Math.max(1, high - low);
    if (index) context.lineTo(x, y); else context.moveTo(x, y);
  });
  context.stroke();
}

async function pollAdc() {
  if (!adcRunning) return;
  const pin = adcPin.value;
  const generation = adcGeneration;
  try {
    const response = await fetch(`/api/adc/raw?gpio=${pin}`, { cache: "no-store" });
    if (!response.ok) throw Error(await response.text());
    const result = await response.json();
    if (!adcRunning || generation !== adcGeneration || pin !== adcPin.value) return;
    adcSamples = result.samples;
    const minimum = Math.min(...adcSamples), maximum = Math.max(...adcSamples);
    const average = Math.round(adcSamples.reduce((sum, value) => sum + value, 0) / adcSamples.length);
    adcStats.textContent = `Min ${minimum} · Átlag ${average} · Max ${maximum} · Csúcs-csúcs ${maximum - minimum}`;
    adcStatus.textContent = `GPIO${pin} · ${adcSamples.length} minta · 12 bit`;
    drawAdc();
  } catch (error) {
    if (generation !== adcGeneration || pin !== adcPin.value) return;
    adcRunning = false;
    adcToggle.textContent = "Mérés indítása";
    adcStatus.textContent = error.message;
  } finally {
    if (adcRunning && generation === adcGeneration) adcTimer = setTimeout(pollAdc, 200);
  }
}

adcToggle.addEventListener("click", () => {
  adcRunning = !adcRunning;
  adcGeneration++;
  adcToggle.textContent = adcRunning ? "Mérés leállítása" : "Mérés indítása";
  if (adcRunning) pollAdc();
  else { clearTimeout(adcTimer); adcStatus.textContent = "Nem mér"; }
});
adcPin.addEventListener("change", () => { adcSamples = []; drawAdc(); if (adcRunning) adcStatus.textContent = "Mérés..."; });
adcScale.addEventListener("change", drawAdc);
window.addEventListener("resize", drawAdc);
document.addEventListener("visibilitychange", () => {
  if (document.hidden && adcRunning) adcToggle.click();
});
drawAdc();
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
    const response = await fetch(pttActive ? "/api/ptt-test/off" : "/api/ptt-test/latch", { method: "POST" });
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