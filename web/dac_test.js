const form = document.getElementById('toneForm');
const starts = [...document.querySelectorAll('.start-tone')];
const stop = document.getElementById('stopTone');
const statusText = document.getElementById('toneStatus');
const progress = document.getElementById('toneProgress');
const radioVolume = document.getElementById('radioVolume');
const radioVolumeValue = document.getElementById('radioVolumeValue');
const radioBoost = document.getElementById('radioBoost');
let message = '';
let pending = false;
let wasTone = false;
let lastDuration = 10;
let adjustingVolume = false;
let adjustingBoost = false;

async function request(path, data) {
  const response = await fetch(path, { method: 'POST', body: data });
  if (!response.ok) throw new Error(await response.text());
}

async function update() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) throw new Error('Status unavailable');
    const state = await response.json();
    const tone = state.playbackPath.startsWith('DAC tone') && state.playback !== 'idle';
    const available = !pending && state.speakerReady && state.playback === 'idle' && state.state === 'idle';
    for (const start of starts) start.disabled = !available;
    stop.disabled = pending || !tone;
    for (const select of form.querySelectorAll('select')) select.disabled = tone || pending;
    if (!adjustingVolume) {
      radioVolume.value = state.radioVolume;
      radioVolumeValue.value = `${state.radioVolume}%`;
    }
    if (!adjustingBoost) radioBoost.value = state.radioBoost;
    progress.hidden = !tone || !state.playbackBytesTotal;
    progress.value = tone && state.playbackBytesTotal ?
      Math.min(100, 100 * state.playbackBytesPlayed / state.playbackBytesTotal) : 0;
    if (tone) {
      statusText.textContent = state.playbackBytesTotal ?
        `${state.playbackPath}: ${Math.round(progress.value)}%` : 'Tone running until stopped';
      wasTone = true;
    } else {
      if (state.playbackError) message = state.playbackError;
      else if (wasTone) message = lastDuration ? 'Test finished.' : 'Test stopped.';
      statusText.textContent = message || (state.speakerReady ? 'Ready' : 'I²S output unavailable');
      wasTone = false;
    }
  } catch (error) {
    for (const start of starts) start.disabled = true;
    stop.disabled = true;
    statusText.textContent = error.message;
  }
}

form.addEventListener('submit', async event => {
  event.preventDefault();
  const duration = event.submitter?.value;
  if (!['10', '30', '60', '0'].includes(duration)) return;
  lastDuration = Number(duration);
  pending = true;
  for (const start of starts) start.disabled = true;
  message = '';
  try {
    const data = new URLSearchParams(new FormData(form));
    data.set('duration', duration);
    await request('/api/dac-tone', data);
  } catch (error) { message = error.message; statusText.textContent = message; }
  finally { pending = false; await update(); }
});

stop.addEventListener('click', async () => {
  pending = true;
  stop.disabled = true;
  try { await request('/api/play/stop'); }
  catch (error) { statusText.textContent = error.message; }
  finally { pending = false; await update(); }
});

radioVolume.addEventListener('input', () => {
  adjustingVolume = true;
  radioVolumeValue.value = `${radioVolume.value}%`;
});
radioVolume.addEventListener('change', async () => {
  adjustingVolume = true;
  try { await request(`/api/play/radio-volume?value=${radioVolume.value}`); }
  catch (error) { statusText.textContent = error.message; }
  finally { adjustingVolume = false; await update(); }
});
radioBoost.addEventListener('change', async () => {
  adjustingBoost = true;
  try { await request(`/api/play/radio-boost?value=${radioBoost.value}`); }
  catch (error) { statusText.textContent = error.message; }
  finally { adjustingBoost = false; await update(); }
});

update();
setInterval(update, 500);