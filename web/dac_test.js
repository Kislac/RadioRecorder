const form = document.getElementById('toneForm');
const start = document.getElementById('startTone');
const stop = document.getElementById('stopTone');
const statusText = document.getElementById('toneStatus');
const progress = document.getElementById('toneProgress');
let message = '';
let pending = false;
let wasTone = false;

async function request(path, data) {
  const response = await fetch(path, { method: 'POST', body: data });
  if (!response.ok) throw new Error(await response.text());
}

async function update() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) throw new Error('Állapot nem érhető el');
    const state = await response.json();
    const tone = state.playbackPath.startsWith('DAC tone') && state.playback !== 'idle';
    const available = !pending && state.speakerReady && state.playback === 'idle' && state.state === 'idle';
    start.disabled = !available;
    stop.disabled = pending || !tone;
    for (const select of form.querySelectorAll('select')) select.disabled = tone || pending;
    progress.value = tone && state.playbackBytesTotal ?
      Math.min(100, 100 * state.playbackBytesPlayed / state.playbackBytesTotal) : 0;
    if (tone) {
      statusText.textContent = `${state.playbackPath}: ${Math.round(progress.value)}%`;
      wasTone = true;
    } else {
      if (state.playbackError) message = state.playbackError;
      else if (wasTone) message = 'A 10 mp-es próba véget ért.';
      statusText.textContent = message || (state.speakerReady ? 'Készen áll' : 'I²S kimenet nem érhető el');
      wasTone = false;
    }
  } catch (error) {
    start.disabled = true;
    stop.disabled = true;
    statusText.textContent = error.message;
  }
}

form.addEventListener('submit', async event => {
  event.preventDefault();
  pending = true;
  start.disabled = true;
  message = '';
  try {
    await request('/api/dac-tone', new URLSearchParams(new FormData(form)));
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

update();
setInterval(update, 500);