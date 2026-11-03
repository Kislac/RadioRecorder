'use strict';

const page = document.body.dataset.page;
const status = (selector, message) => { document.querySelector(selector).textContent = message; };

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
  const update = () => {
    document.querySelector('#autoSummary').textContent = radio.checked || mic.checked ? '● MINTA ÉLESÍTVE' : '● MINDKETT KI';
    status('#autoStatus', 'Helyi minta: rádió ' + (radio.checked ? 'bekapcsolva' : 'kikapcsolva') + ', mikrofon ' + (mic.checked ? 'bekapcsolva' : 'kikapcsolva') + '. Nem indul felvétel.');
  };
  radio.addEventListener('change', update);
  mic.addEventListener('change', update);
  document.querySelector('#autoPriority').addEventListener('change', event => { document.querySelector('#priorityValue').textContent = event.target.value === 'radio' ? 'RÁDIÓ → MIKROFON' : 'MIKROFON → RÁDIÓ'; });
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