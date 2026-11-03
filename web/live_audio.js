const liveToggle = document.querySelector("#liveToggle"),
  liveStatus = document.querySelector("#liveStatus"),
  liveVolume = document.querySelector("#liveVolume"),
  liveTest = document.querySelector("#liveTest");
let liveSocket = null,
  audioContext = null,
  audioGain = null,
  nextAudioTime = 0;

function stopLive() {
  const socket = liveSocket;
  liveSocket = null;
  if (socket) socket.close();
  const context = audioContext;
  audioContext = null;
  audioGain = null;
  if (context) context.close().catch(() => {});
  liveToggle.textContent = "Start listening";
  liveStatus.textContent = "Disabled";
}
liveToggle.addEventListener("click", async () => {
  if (liveSocket) {
    stopLive();
    return;
  }
  liveToggle.disabled = true;
  try {
    const Context = window.AudioContext || window.webkitAudioContext;
    if (!Context || !window.WebSocket) throw Error();
    audioContext = new Context({ latencyHint: "interactive" });
    audioGain = audioContext.createGain();
    audioGain.gain.value = Number(liveVolume.value) / 100;
    audioGain.connect(audioContext.destination);
    const unlock = audioContext.createBufferSource();
    unlock.buffer = audioContext.createBuffer(1, 1, 22050);
    unlock.connect(audioGain);
    unlock.start();
    await audioContext.resume();
    const socket = new WebSocket(window.radioRecorderAudioUrl || "ws://" + location.hostname + ":81/");
    socket.binaryType = "arraybuffer";
    liveSocket = socket;
    liveStatus.textContent = "Connecting...";
    socket.onopen = () => {
      if (socket !== liveSocket) return;
      liveToggle.textContent = "Stop listening";
      liveStatus.textContent = "Waiting for audio...";
    };
    socket.onmessage = (event) => {
      if (socket !== liveSocket || !(event.data instanceof ArrayBuffer) || !audioContext) return;
      const view = new DataView(event.data),
        length = Math.floor(view.byteLength / 2);
      if (!length) return;
      const buffer = audioContext.createBuffer(1, length, 22050),
        channel = buffer.getChannelData(0);
      let peak = 0;
      for (let index = 0; index < length; index++) {
        const value = view.getInt16(index * 2, true) / 32768;
        channel[index] = value;
        peak = Math.max(peak, Math.abs(value));
      }
      if (audioContext.state !== "running") {
        liveStatus.textContent = "Playback paused (" + audioContext.state + "), PCM peak: " + Math.round(peak * 100) + "%";
        return;
      }
      const now = audioContext.currentTime;
      if (nextAudioTime < now + 0.12 || nextAudioTime > now + 0.8) nextAudioTime = now + 0.12;
      const source = audioContext.createBufferSource();
      source.buffer = buffer;
      source.connect(audioGain);
      source.start(nextAudioTime);
      nextAudioTime += buffer.duration;
      liveStatus.textContent = "Live audio, PCM peak: " + Math.round(peak * 100) + "%, kb. " + Math.round((nextAudioTime - now) * 1000) + " ms puffer";
    };
    socket.onclose = () => {
      if (socket !== liveSocket) return;
      stopLive();
      liveStatus.textContent = "Connection closed.";
    };
    socket.onerror = () => {
      if (socket !== liveSocket) return;
      stopLive();
      liveStatus.textContent = "Audio connection unavailable.";
    };
  } catch {
    stopLive();
    liveStatus.textContent = "The browser could not start audio.";
  }
  liveToggle.disabled = false;
});
liveVolume.addEventListener("input", () => {
  if (audioGain) audioGain.gain.value = Number(liveVolume.value) / 100;
});
liveTest.addEventListener("click", async () => {
  const Context = window.AudioContext || window.webkitAudioContext;
  if (!Context) {
    liveStatus.textContent = "Audio test unsupported.";
    return;
  }
  const context = audioContext || new Context({ latencyHint: "interactive" });
  try {
    const tone = context.createOscillator(), gain = context.createGain();
    tone.frequency.value = 440;
    gain.gain.value = (Number(liveVolume.value) / 100) * 0.15;
    tone.connect(gain);
    gain.connect(context.destination);
    tone.onended = () => {
      tone.disconnect();
      gain.disconnect();
      if (context !== audioContext) context.close().catch(() => {});
    };
    tone.start();
    tone.stop(context.currentTime + 0.4);
    await context.resume();
    liveStatus.textContent = "Audio test played.";
  } catch {
    if (context !== audioContext) context.close().catch(() => {});
    liveStatus.textContent = "Audio test failed.";
  }
});
addEventListener("pagehide", stopLive);

async function refreshLiveStatus() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) throw Error();
    const data = await response.json();
    liveToggle.disabled = !liveSocket && (!data.liveReady || data.playback !== "idle");
    liveTest.disabled = !!data.automatic?.microphone?.armed;
  } catch {
    liveToggle.disabled = !liveSocket;
    liveTest.disabled = true;
  }
}
refreshLiveStatus();
setInterval(refreshLiveStatus, 1500);