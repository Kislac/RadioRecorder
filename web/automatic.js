(() => {
  const codes = ["manualMic", "manualRadio", "autoRadio", "autoMic"];
  const labels = {manualMic:"Manual mic", manualRadio:"Manual radio", autoRadio:"Auto radio", autoMic:"Auto mic"};
  const elapsedText = milliseconds => {
    const seconds = Math.floor(Math.max(0, milliseconds || 0) / 1000);
    return [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60].map(value => String(value).padStart(2, "0")).join(":");
  };

  window.renderAutomaticRecording = status => {
    if (!status) return;
    for (const [name, prefix] of [["radio", "autoRadio"], ["microphone", "autoMic"]]) {
      const source = status[name];
      const checkbox = document.querySelector("#" + prefix + "Armed");
      if (!source || !checkbox) continue;
      if (!checkbox.disabled) checkbox.checked = source.armed;
      document.querySelector("#" + prefix + "Config").textContent = "RMS threshold: " + source.threshold + " · Silence: " + source.silenceSeconds + " s";
      document.querySelector("#" + prefix + "Status").textContent = source.error || (source.armed ? source.blocked : "") || (source.recording ? "Recording: YES" : "Recording: NO");
      document.querySelector("#" + prefix + "Time").textContent = "Time: " + elapsedText(source.recording ? source.elapsedMs : source.lastElapsedMs);
      document.querySelector("#" + prefix + "File").textContent = source.lastFile || "";
    }
  };

  for (const [source, id] of [["radio", "autoRadioArmed"], ["microphone", "autoMicArmed"]]) {
    const checkbox = document.querySelector("#" + id);
    if (!checkbox) continue;
    checkbox.addEventListener("change", async () => {
      const requested = checkbox.checked;
      checkbox.disabled = true;
      try {
        const response = await fetch("/api/auto-record/arm?" + new URLSearchParams({source, enabled:checkbox.checked ? "1" : "0"}), {method:"POST"});
        if (!response.ok) throw Error(await response.text());
        const status = await response.json(); checkbox.checked = status[source].armed;
        window.renderAutomaticRecording(status);
      } catch (error) {
        checkbox.checked = !requested;
        document.querySelector("#" + (source === "radio" ? "autoRadioStatus" : "autoMicStatus")).textContent = error.message;
      } finally { checkbox.disabled = false; }
    });
  }

  const form = document.querySelector("#autoSettingsForm");
  if (!form) return;
  let dirty = false, loaded = false, draft = [...codes];
  const selectors = codes.map((_, index) => document.querySelector("#recordPriority" + index));
  for (const [index, selector] of selectors.entries()) {
    selector.replaceChildren(...codes.map(code => new Option(labels[code], code)));
    selector.addEventListener("change", () => {
      const other = draft.indexOf(selector.value), previous = draft[index];
      draft[index] = selector.value;
      if (other !== index) { draft[other] = previous; selectors[other].value = previous; }
      dirty = true;
    });
  }
  form.addEventListener("input", () => { dirty = true; });

  async function loadSettings() {
    try {
      const response = await fetch("/api/auto-record", {cache:"no-store"});
      if (!response.ok) throw Error();
      const data = await response.json();
      if (!dirty) {
        for (const [source, prefix] of [["radio", "autoRadio"], ["microphone", "autoMic"]]) {
          document.querySelector("#" + prefix + "Threshold").value = data[source].threshold;
          document.querySelector("#" + prefix + "Silence").value = data[source].silenceSeconds;
        }
        draft = [...data.priorities]; selectors.forEach((selector, index) => { selector.value = draft[index]; });
      }
      document.querySelector("#saveAutoSettings").disabled = false;
      const result = document.querySelector("#autoSettingsResult");
      if (!loaded) result.textContent = "Settings loaded.";
      loaded = true;
    } catch { loaded = false; document.querySelector("#autoSettingsResult").textContent = "Automatic settings unavailable"; }
  }

  form.addEventListener("submit", async event => {
    event.preventDefault(); if (!form.reportValidity()) return;
    const button = document.querySelector("#saveAutoSettings"); button.disabled = true;
    const data = {priorities:draft};
    for (const [source, prefix] of [["radio", "autoRadio"], ["microphone", "autoMic"]]) data[source] = {
      threshold:Number(document.querySelector("#" + prefix + "Threshold").value),
      silenceSeconds:Number(document.querySelector("#" + prefix + "Silence").value),
    };
    try {
      const response = await fetch("/api/auto-record/settings", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(data)});
      if (!response.ok) throw Error(await response.text());
      await response.json(); dirty = false; document.querySelector("#autoSettingsResult").textContent = "Settings saved.";
      await loadSettings();
    } catch (error) { document.querySelector("#autoSettingsResult").textContent = error.message; }
    finally { button.disabled = false; }
  });
  loadSettings(); setInterval(loadSettings, 3000);
})();