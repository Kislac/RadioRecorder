(() => {
  function syncPhoneClock() {
    if (document.visibilityState !== "visible") return;
    const epoch = Math.floor(Date.now() / 1000);
    fetch("/api/time/auto?" + new URLSearchParams({ epoch }), { method: "POST" }).catch(() => {});
  }
  syncPhoneClock();
  setInterval(syncPhoneClock, 300000);
  document.addEventListener("visibilitychange", syncPhoneClock);
})();