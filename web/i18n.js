(() => {
  const catalogs = window.RR_LANGUAGES;
  let language = "hu";
  let patterns = [];
  const originals = new WeakMap();
  const attributes = new WeakMap();
  const excluded = "script,style,textarea,input,code,pre,[data-i18n-ignore]";

  function translate(text) {
    const leading = text.match(/^\s*/)[0];
    const trailing = text.match(/\s*$/)[0];
    const trimmed = text.trim();
    if (trimmed.startsWith("[Offline] ")) return leading + "[Offline] " + translate(trimmed.slice(10)) + trailing;
    const catalog = catalogs[language].web;
    if (Object.hasOwn(catalog, trimmed)) return leading + catalog[trimmed] + trailing;
    for (const {expression, names, replacement} of patterns) {
      const match = trimmed.match(expression);
      if (!match) continue;
      return leading + replacement.replace(/\{(\w+)\}/g, (token, name) => match[names.indexOf(name) + 1]) + trailing;
    }
    return text;
  }

  function translateNode(node) {
    if (!node.parentElement || node.parentElement.closest(excluded)) return;
    const current = node.textContent;
    const saved = originals.get(node);
    const original = saved && saved.rendered === current ? saved.original : current;
    const rendered = translate(original);
    originals.set(node, {original, rendered});
    if (current !== rendered) node.textContent = rendered;
  }

  function render() {
    const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) translateNode(walker.currentNode);
    for (const element of document.querySelectorAll("[title],[aria-label],[placeholder]")) {
      if (element.closest("script,style,[data-i18n-ignore]")) continue;
      const saved = attributes.get(element) || {};
      for (const name of ["title", "aria-label", "placeholder"]) {
        if (!element.hasAttribute(name)) continue;
        const current = element.getAttribute(name);
        const previous = saved[name];
        const original = previous && previous.rendered === current ? previous.original : current;
        const rendered = translate(original);
        saved[name] = {original, rendered};
        if (current !== rendered) element.setAttribute(name, rendered);
      }
      attributes.set(element, saved);
    }
  }

  function applyLanguage(code) {
    if (!Object.hasOwn(catalogs, code)) return false;
    language = code;
    patterns = [];
    for (const [original, replacement] of Object.entries(catalogs[code].web)) {
      if (!original.includes("{")) continue;
      const names = [];
      const expression = original.split(/(\{\w+\})/g).map(part => {
        if (/^\{\w+\}$/.test(part)) {
          names.push(part.slice(1, -1));
          return "(.*?)";
        }
        return part.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
      }).join("");
      patterns.push({expression: new RegExp("^" + expression + "$"), names, replacement});
    }
    patterns.sort((first, second) => second.expression.source.length - first.expression.source.length);
    document.documentElement.lang = code;
    const selector = document.querySelector("#language");
    if (selector) selector.value = code;
    render();
    return true;
  }

  async function syncLanguage() {
    try {
      const response = await fetch("/api/settings", {cache: "no-store"});
      if (!response.ok) return;
      const data = await response.json();
      if (data.language && data.language !== language) applyLanguage(data.language);
    } catch {}
  }

  window.RR_I18N = {translate, applyLanguage};
  const originalConfirm = window.confirm.bind(window);
  window.confirm = text => originalConfirm(translate(text));
  const originalAlert = window.alert.bind(window);
  window.alert = text => originalAlert(translate(text));

  document.addEventListener("DOMContentLoaded", () => {
    const selector = document.querySelector("#language");
    if (selector) {
      selector.replaceChildren(...Object.entries(catalogs).map(([code, catalog]) => new Option(catalog.name, code)));
      selector.addEventListener("change", async () => {
        selector.disabled = true;
        try {
          const response = await fetch("/api/language?" + new URLSearchParams({language: selector.value}), {method: "POST"});
          if (!response.ok) throw Error();
          applyLanguage((await response.json()).language);
        } catch {
          selector.value = language;
          document.querySelector("#languageStatus").textContent = translate("Could not save language.");
        } finally {
          selector.disabled = false;
        }
      });
    }
    applyLanguage(language);
    const observer = new MutationObserver(() => {
      observer.disconnect();
      render();
      observer.observe(document.documentElement, {childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ["title", "aria-label", "placeholder"]});
    });
    observer.observe(document.documentElement, {childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ["title", "aria-label", "placeholder"]});
    syncLanguage();
    setInterval(syncLanguage, 5000);
  });
})();