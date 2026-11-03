'use strict';

const page = document.body.dataset.page;
const status = (selector, message) => { document.querySelector(selector).textContent = message; };
const defaultAutoRules = { radio: { enabled: true, threshold: 140, silence: 3 }, mic: { enabled: true, threshold: 700, silence: 5 }, priority: 'radio' };
const readAutoRules = () => {
  try {
    const saved = JSON.parse(sessionStorage.getItem('demoAutoRules') || 'null');
    return { radio: { ...defaultAutoRules.radio, ...saved?.radio }, mic: { ...defaultAutoRules.mic, ...saved?.mic }, priority: saved?.priority || 'radio' };
  } catch { return defaultAutoRules; }
};

if (page === 'home') {
  const modeInputs = [...document.querySelectorAll('input[name="recordMode"]')];
  const sourceInputs = [...document.querySelectorAll('input[name="source"]')];
  const button = document.querySelector('#recordButton');
  const trace = document.querySelector('#historyTrace');
  const spectrum = document.querySelector('#spectrum');
  const frequencies = ['125', '250', '500', '1k', '2k', '4k', '8k', '16k'];
  const bars = frequencies.map(frequency => {
    const band = document.createElement('div'); band.className = 'spectrum-band';
    const bar = document.createElement('span'); bar.className = 'spectrum-bar';
    const label = document.createElement('small'); label.textContent = frequency;
    band.append(bar, label); spectrum.append(band);
    return bar;
  });
  let active = false;
  let recording = false;
  let quietSince = 0;
  let timer;
  let frame = 0;
  let history = [];
  const selectedMode = () => modeInputs.find(input => input.checked).value;
  const selectedSource = () => sourceInputs.find(input => input.checked).value;
  const refresh = () => {
    const source = selectedSource();
    const automatic = selectedMode() === 'auto';
    const rule = readAutoRules()[source];
    const label = source === 'radio' ? 'Rádió' : 'Mikrofon';
    status('#signalSource', label.toUpperCase() + ' / JELMONITOR');
    status('#autoConfig', label + ': ' + (rule.enabled ? 'küszöb ' + rule.threshold + ', csendidő ' + rule.silence + ' mp.' : 'az automatikus szabály ki van kapcsolva.'));
    button.disabled = !active && automatic && !rule.enabled;
    status('#recordAction', active ? 'Leállítás' : automatic ? 'Automatikus figyelés indítása' : 'Kézi felvétel indítása');
    status('#modeInfo', automatic ? (rule.enabled ? 'Mintajel alapján, ' + label.toLowerCase() + ' bemeneten.' : 'Kapcsold be a szabályt az Automata beállítások oldalon.') : 'Egyidejűleg egy forrás rögzíthető.');
  };
  const tick = () => {
    frame += 1;
    const source = selectedSource();
    const level = Math.min(1, .12 + .55 * Math.abs(Math.sin(frame * .19 + (source === 'mic' ? 1.4 : 0))) + .2 * Math.abs(Math.sin(frame * .53)));
    const rms = Math.round(level * (source === 'radio' ? 270 : 1300));
    const peak = Math.round(rms * 1.35);
    history.push(level);
    if (history.length > 64) history.shift();
    trace.setAttribute('d', history.map((value, index) => (index ? 'L' : 'M') + (index * 720 / 63).toFixed(1) + ' ' + (104 - value * 92).toFixed(1)).join(' '));
    bars.forEach((bar, index) => { bar.style.height = Math.min(95, Math.round(12 + level * (55 + 25 * Math.sin(index * 1.3 + frame * .17)) + 14 * Math.abs(Math.sin(frame * .31 + index)))) + '%'; });
    status('#signalLevel', 'RMS ' + rms + ' · PEAK ' + peak);
    status('#spectrumTime', String(Math.floor(frame / 4 / 60)).padStart(2, '0') + ':' + String(Math.floor(frame / 4) % 60).padStart(2, '0'));
    if (selectedMode() === 'auto') {
      const rule = readAutoRules()[source];
      if (rms >= rule.threshold) { quietSince = 0; recording = true; }
      else if (recording) {
        if (!quietSince) quietSince = Date.now();
        if (Date.now() - quietSince >= rule.silence * 1000) recording = false;
      }
    }
    status('#recordState', recording ? '● MINTAFELVÉTEL' : '● FIGYELÉS');
    status('#recordStatus', recording ? 'Szimulált ' + (selectedMode() === 'auto' ? 'automatikus' : 'kézi') + ' felvétel · nincs SD-írás.' : 'Figyelés aktív · jel a beállított küszöb alatt.');
  };
  button.addEventListener('click', () => {
    if (active) {
      clearInterval(timer); active = false; recording = false; button.classList.remove('is-active');
      [...modeInputs, ...sourceInputs].forEach(input => { input.disabled = false; });
      status('#recordState', '● KÉSZENLÉT');
      status('#recordStatus', 'Mintaművelet leállítva · nincs mentett felvétel.');
      refresh();
      return;
    }
    active = true; recording = selectedMode() === 'manual'; quietSince = 0; frame = 0; history = [];
    [...modeInputs, ...sourceInputs].forEach(input => { input.disabled = true; });
    button.classList.add('is-active'); refresh(); tick(); timer = setInterval(tick, 250);
  });
  [...modeInputs, ...sourceInputs].forEach(input => input.addEventListener('change', refresh));
  refresh();
}

if (page === 'files') {
  const entries = [
    { name: '2026-11-02', type: 'folder', date: 3 },
    { name: '2026-11-01', type: 'folder', date: 2 },
    { name: 'RadioMan261102_142814.wav', type: 'file', date: 3, folder: '2026-11-02', detail: '14:28 · 02:36 · 5,2 MB' },
    { name: 'MicMan261102_100304.wav', type: 'file', date: 3, folder: '2026-11-02', detail: '10:03 · 00:48 · 1,7 MB' },
    { name: 'RadioMan261101_083012.wav', type: 'file', date: 2, folder: '2026-11-01', detail: '08:30 · 01:22 · 2,8 MB' },
  ];
  let folder = '';
  let playbackTimer;
  const list = document.querySelector('#fileList');
  const search = document.querySelector('#fileSearch');
  const progress = document.querySelector('#playbackProgress');
  const render = () => {
    document.querySelector('#filePath').textContent = '/' + (folder ? folder + '/' : '');
    list.replaceChildren();
    const filtered = entries.filter(entry => (folder ? entry.folder === folder : entry.type === 'folder') && entry.name.toLowerCase().includes(search.value.toLowerCase()));
    filtered.sort((left, right) => document.querySelector('#fileSort').value === 'name' ? left.name.localeCompare(right.name) : right.date - left.date || left.name.localeCompare(right.name));
    for (const entry of filtered) {
      const row = document.createElement('div');
      row.className = 'entry';
      const icon = document.createElement('span'); icon.className = 'entry-icon'; icon.textContent = entry.type === 'folder' ? '▣' : '♪';
      const name = document.createElement('div'); name.className = 'entry-name'; name.textContent = entry.name;
      const detail = document.createElement('small'); detail.textContent = entry.type === 'folder' ? 'Dátummappa' : entry.detail; name.append(detail);
      const actions = document.createElement('div'); actions.className = 'entry-actions';
      const button = document.createElement('button'); button.type = 'button'; button.className = 'command secondary';
      button.textContent = entry.type === 'folder' ? 'Megnyitás' : '▶ Próba';
      button.addEventListener('click', () => {
        if (entry.type === 'folder') { folder = entry.name; search.value = ''; render(); }
        else { clearInterval(playbackTimer); progress.value = 0; status('#fileStatus', 'Mintalejátszás: ' + entry.name + ' (nincs hang).'); playbackTimer = setInterval(() => { progress.value = Math.min(100, Number(progress.value) + 10); if (Number(progress.value) === 100) clearInterval(playbackTimer); }, 250); }
      });
      actions.append(button); row.append(icon, name, actions); list.append(row);
    }
    if (!filtered.length) { const empty = document.createElement('p'); empty.className = 'muted'; empty.textContent = 'Nincs találat ebben a mappában.'; list.append(empty); }
  };
  search.addEventListener('input', render);
  document.querySelector('#fileSort').addEventListener('change', render);
  document.querySelector('#fileBack').addEventListener('click', () => { folder = ''; search.value = ''; render(); });
  document.querySelector('#stopPlayback').addEventListener('click', () => { clearInterval(playbackTimer); progress.value = 0; status('#fileStatus', 'Mintalejátszás leállítva.'); });
  render();
}

if (page === 'auto') {
  const radio = document.querySelector('#armRadio');
  const mic = document.querySelector('#armMic');
  const rules = readAutoRules();
  radio.checked = rules.radio.enabled;
  mic.checked = rules.mic.enabled;
  for (const [source, prefix] of [['radio', 'radio'], ['mic', 'mic']]) {
    document.querySelector('#' + prefix + 'Threshold').value = rules[source].threshold;
    document.querySelector('#' + prefix + 'Silence').value = rules[source].silence;
  }
  document.querySelector('#autoPriority').value = rules.priority;
  const update = () => {
    const values = ['radioThreshold', 'radioSilence', 'micThreshold', 'micSilence'].map(id => document.querySelector('#' + id));
    if (values.some(input => !input.checkValidity())) { status('#autoStatus', 'A küszöb és a csendidő csak a megadott tartományban menthető.'); return; }
    rules.radio = { enabled: radio.checked, threshold: Number(values[0].value), silence: Number(values[1].value) };
    rules.mic = { enabled: mic.checked, threshold: Number(values[2].value), silence: Number(values[3].value) };
    rules.priority = document.querySelector('#autoPriority').value;
    let saved = true;
    try { sessionStorage.setItem('demoAutoRules', JSON.stringify(rules)); } catch { saved = false; }
    document.querySelector('#autoSummary').textContent = radio.checked || mic.checked ? '● SZABÁLYOK AKTÍVAK' : '● MINDKETT KI';
    document.querySelector('#priorityValue').textContent = rules.priority === 'radio' ? 'RÁDIÓ → MIKROFON' : 'MIKROFON → RÁDIÓ';
    status('#autoStatus', saved ? 'Mintaszabályok mentve erre a böngészőlapra. A figyelést a Rögzítés oldalon indíthatod.' : 'A böngésző nem engedi a mintaszabályok tárolását.');
  };
  [radio, mic, ...document.querySelectorAll('#radioThreshold, #radioSilence, #micThreshold, #micSilence, #autoPriority')].forEach(input => input.addEventListener('change', update));
  update();
}

if (page === 'settings') {
  document.querySelector('#settingsForm').addEventListener('submit', event => { event.preventDefault(); status('#settingsStatus', 'A mintabeállításokat ezen az oldalon alkalmaztuk; újratöltéskor visszaállnak.'); });
  document.querySelector('#clockApply').addEventListener('click', () => { const clock = document.querySelector('#clockLocal'); status('#clockStatus', clock.value ? 'Mintakijelzés: ' + clock.value.replace('T', ' ') : 'Előbb adj meg dátumot.'); });
  document.querySelector('#scanNetworks').addEventListener('click', () => {
    const container = document.querySelector('#networks'); container.replaceChildren();
    for (const name of ['Bázis-01', 'Bázis-02']) {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'command secondary'; button.textContent = name;
      button.addEventListener('click', () => { document.querySelector('#staInfo').textContent = name + (name === 'Bázis-01' ? ' / 192.168.0.52' : ' / 192.168.0.53'); status('#wifiStatus', name + ' kiválasztva mintaként. Nincs valódi csatlakozás.'); });
      container.append(button);
    }
    status('#wifiStatus', 'Két mintahálózat található. Nincs valódi hálózatkeresés.');
  });
}

if (page === 'radio') {
  let readTimer;
  document.querySelector('#radioRead').addEventListener('click', () => {
    clearInterval(readTimer);
    const progress = document.querySelector('#radioProgress'); progress.value = 0;
    document.querySelector('#radioResult').hidden = true;
    status('#radioStatus', 'Fiktív csatornák mintakiolvasása...');
    readTimer = setInterval(() => {
      progress.value = Math.min(100, Number(progress.value) + 20);
      if (Number(progress.value) === 100) { clearInterval(readTimer); document.querySelector('#radioResult').hidden = false; status('#radioStatus', 'Két fiktív csatorna betöltve. Nincs valódi rádiókapcsolat.'); }
    }, 180);
  });
}

if (page === 'hardware') {
  const bars = document.querySelector('#adcBars');
  for (let index = 0; index < 24; index += 1) bars.append(document.createElement('span'));
  let measuring = false;
  document.querySelector('#adcToggle').addEventListener('click', event => {
    measuring = !measuring;
    event.target.textContent = measuring ? 'Mintamérés leállítása' : 'Mintamérés indítása';
    status('#adcStatus', measuring ? 'Szimulált jel: GPIO' + document.querySelector('#adcPin').value + ' · min 1850 / átlag 2040 / max 2210' : 'Nem mér. A grafikon csak helyi illusztráció.');
  });
  document.querySelector('#adcPin').addEventListener('change', () => { if (measuring) status('#adcStatus', 'Szimulált jel: GPIO' + document.querySelector('#adcPin').value + ' · min 1850 / átlag 2040 / max 2210'); });
}

if (page === 'dac') {
  let toneTimer;
  const progress = document.querySelector('#toneProgress');
  const stop = () => { clearInterval(toneTimer); toneTimer = undefined; progress.value = 0; status('#toneStatus', 'A szimuláció leállt. Hang nem szólt.'); };
  document.querySelector('#toneStop').addEventListener('click', stop);
  document.querySelector('#toneForm').addEventListener('submit', event => {
    event.preventDefault(); clearInterval(toneTimer); progress.value = 0;
    status('#toneStatus', document.querySelector('#frequency').value + ' mintaindítás. Hang nem szól és a készülék nem érhető el.');
    toneTimer = setInterval(() => { progress.value = Math.min(100, Number(progress.value) + 10); if (Number(progress.value) === 100) { clearInterval(toneTimer); toneTimer = undefined; status('#toneStatus', 'A szimuláció befejeződött. Hang nem szólt.'); } }, 250);
  });
}