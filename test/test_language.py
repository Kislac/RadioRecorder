import json
from html.parser import HTMLParser
import os
import re
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from scripts.preview_web import PreviewDevice
from scripts.test_device import test_language_selection
from scripts.web_assets import ROOT, PAGES, generate_languages, load_languages, render_page


class LanguageTests(unittest.TestCase):
    def test_replay_labels_and_statuses_have_hungarian_translations(self):
        class Labels(HTMLParser):
            def __init__(self):
                super().__init__()
                self.labels = []

            def handle_data(self, data):
                if data.strip():
                    self.labels.append(data.strip())

            def handle_starttag(self, tag, attrs):
                self.labels.extend(value for name, value in attrs if name in ("title", "aria-label"))

        labels = Labels()
        labels.feed((ROOT / "web/automatic_replay.html").read_text(encoding="utf-8"))
        catalog = load_languages()["hu"]["web"]
        script = (ROOT / "web/automatic_replay.js").read_text(encoding="utf-8")
        self.assertIn("toLocaleString(document.documentElement.lang)", script)
        proper_names = {"RADIORECORDER", "CLEAR SKY / SIGNAL STATION", "microSD", "ESP32-S3 / Radio Recorder"}
        self.assertEqual({text for text in labels.labels if text not in catalog and text not in proper_names
                          and any(letter.isalpha() for letter in text)}, set())
        controller = (ROOT / "src/automatic_replay.cpp").read_text(encoding="utf-8")
        server = (ROOT / "src/web_server.cpp").read_text(encoding="utf-8")
        errors = set(re.findall(r'(?:replayError|error) = "([^"]+)"', controller))
        routes = server.split('server.on("/api/automatic-replay/rules"', 1)[1].split('server.on("/api/auto-record/settings"', 1)[0]
        errors.update(re.findall(r'server.send\(\d+, "text/plain", "([^"]+)"\)', routes))
        self.assertEqual({text for text in errors if text not in catalog or catalog[text] == text}, set())
        for key in ("Automatic replay", "Disarmed", "Awaiting local confirmation", "{count} waiting",
                    "Completed", "Inactive", "Folder (", "{count} WAVs,", "start", "Every {minutes} min",
                    "Next: {date}", "Activate", "Deactivate", "Edit", "Invalid schedule",
                    "Delay must be 1 second to 365 days", "Confirm on the encoder within 30 seconds"):
            with self.subTest(label=key):
                self.assertIn(key, catalog)
                self.assertNotEqual(catalog[key], key)

    def test_duplicate_catalog_keys_are_rejected_even_with_equal_values(self):
        catalogs = load_languages()
        with TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "locales").mkdir()
            for code, catalog in catalogs.items():
                (root / "locales" / (code + ".json")).write_text(json.dumps(catalog), encoding="utf-8")
            for code in ("en", "hu"):
                with self.subTest(language=code):
                    path = root / "locales" / (code + ".json")
                    original = json.dumps(catalogs[code])
                    value = json.dumps(catalogs[code]["web"]["Settings"])
                    path.write_text(original.replace('"web": {', '"web": {"Settings": ' + value + ',', 1), encoding="utf-8")
                    with patch("scripts.web_assets.ROOT", root), self.assertRaisesRegex(ValueError, "Duplicate language catalog key: Settings"):
                        load_languages()
                    path.write_text(original, encoding="utf-8")

    def test_new_catalog_is_discovered_and_invalid_catalogs_are_rejected(self):
        catalogs = load_languages()
        with TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "locales").mkdir()
            for code, catalog in catalogs.items():
                (root / "locales" / (code + ".json")).write_text(json.dumps(catalog), encoding="utf-8")
            extra = json.loads(json.dumps(catalogs["en"]))
            extra["name"] = "Portuguese"
            path = root / "locales" / "pt-BR.json"
            path.write_text(json.dumps(extra), encoding="utf-8")
            with patch("scripts.web_assets.ROOT", root):
                self.assertIn("pt-BR", load_languages())
                generate_languages(root / "language.generated.h")
                self.assertIn('"pt-BR", "Portuguese", kLanguage_pt_BR',
                              (root / "language.generated.h").read_text(encoding="utf-8"))
                extra["oled"]["Source: %s"] = "Source: %u"
                path.write_text(json.dumps(extra), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "placeholders differ"):
                    load_languages()
                del extra["oled"]["Source: %s"]
                path.write_text(json.dumps(extra), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Incomplete"):
                    load_languages()

    def test_serial_language_probe_restores_language_and_navigation(self):
        state = {"tile": 2, "setting": 0, "settings": False, "language": "hu"}

        def write(command):
            if command == b"l":
                field = "setting" if state["settings"] else "tile"
                state[field] = (state[field] + 1) % (25 if state["settings"] else 12)
            elif command == b"b":
                state["settings"] = False
            elif command == b"k":
                if not state["settings"]:
                    self.assertEqual(state["tile"], 11)
                    state["settings"] = True
                else:
                    self.assertEqual(state["setting"], 14)
                    state["language"] = "en" if state["language"] == "hu" else "hu"

        def frame(device):
            pixels = bytearray(1024)
            pixels[256] = 1 if state["language"] == "hu" else 2
            return bytes(pixels)

        device = SimpleNamespace(write=write)
        with patch("scripts.test_device.menu_state", side_effect=lambda device:
                   (state["tile"], state["setting"], int(state["settings"]), 0, 0)), \
                patch("scripts.test_device.language_snapshot", side_effect=lambda device, report: state["language"]), \
                patch("scripts.test_device.read_frame", side_effect=frame):
            self.assertIn("original language and navigation restored", test_language_selection(device, None))
        self.assertEqual((state["tile"], state["settings"], state["language"]), (2, False, "hu"))

    def test_catalogs_are_complete_and_embedded_on_every_page(self):
        catalogs = load_languages()
        self.assertEqual(set(catalogs), {"hu", "en"})
        for section in ("oled", "web"):
            self.assertTrue(all(key == value for key, value in catalogs["en"][section].items()),
                            "English catalog keys must be English source text")
        for code, catalog in catalogs.items():
            self.assertEqual(set(catalog["oled"]), set(catalogs["hu"]["oled"]))
            self.assertEqual(set(catalog["web"]), set(catalogs["hu"]["web"]))
            for key, value in catalog["oled"].items():
                with self.subTest(code=code, key=key):
                    self.assertTrue(value.isascii())
                    rendered = re.sub(r"%[-+0 #]*\d*(?:\.\d+)?(?:ll|l|h|z)?[a-z]", "00", value)
                    self.assertLessEqual(len(rendered), 25)
        self.assertEqual(catalogs["en"]["oled"]["Settings"], "Settings")
        self.assertEqual(catalogs["hu"]["oled"]["Time"], "Ido")
        display = (ROOT / "src/display.cpp").read_text(encoding="utf-8")
        self.assertIn("oled.drawUTF8(x, y, appText(text))", display)
        self.assertIn("kSettingsCount = 25", display)
        self.assertIn('case 14: drawText(9, y, "Language")', display)
        self.assertIn('appSetLanguage(appNextLanguage())', display)
        for name in PAGES:
            self.assertIn("window.RR_LANGUAGES", render_page(name))
            self.assertIn("/api/language?", render_page(name))

    def test_program_sources_do_not_embed_hungarian_display_text(self):
        catalogs = load_languages()
        translations = {value for section in ("oled", "web")
                        for key, value in catalogs["hu"][section].items()
                        if key != value and len(value) > 2}
        for folder, patterns in (("src", ("*.cpp",)), ("scripts", ("*.py",)),
                                 ("web", ("*.html", "*.js"))):
            for pattern in patterns:
                for path in (ROOT / folder).glob(pattern):
                    source = path.read_text(encoding="utf-8")
                    with self.subTest(path=path.name):
                        self.assertIsNone(re.search(r"[áéíóöőúüűÁÉÍÓÖŐÚÜŰ]", source))
                        for text in translations:
                            self.assertNotIn(json.dumps(text, ensure_ascii=False), source)
                            self.assertNotIn("'" + text + "'", source)

    def test_language_browser_translation_restores_originals_and_preserves_values(self):
        node = shutil.which("node")
        if node is None:
            self.skipTest("Node.js required for browser language regression")
        script = r"""
const assert = require('node:assert/strict');
const fs = require('node:fs');
const catalogs = Object.fromEntries(['hu','en'].map(code => [code, JSON.parse(fs.readFileSync('locales/' + code + '.json','utf8'))]));
const nodes = [
  {textContent:'Settings'},
  {textContent:'  Start recording  '},
    {textContent:'GPIO1 signal'},
  {textContent:'RMS: 10 | Peak: 20 (PCM)'},
  {textContent:'Settings', ignored:true},
];
for (const node of nodes) node.parentElement = {closest: () => node.ignored ? {} : null};
const document = {
 documentElement:{lang:'hu'},
 createTreeWalker:() => { let index = -1; return {nextNode:() => ++index < nodes.length, get currentNode() {return nodes[index];}}; },
 querySelector:() => null,
 querySelectorAll:() => [],
 addEventListener:() => {},
};
const NodeFilter = {SHOW_TEXT:4};
const window = {RR_LANGUAGES:catalogs,confirm:text=>text,alert:text=>text};
eval(fs.readFileSync('web/i18n.js','utf8'));
assert.equal(window.RR_I18N.applyLanguage('en'), true);
assert.equal(document.documentElement.lang, 'en');
assert.equal(nodes[0].textContent, 'Settings');
assert.equal(nodes[1].textContent, '  Start recording  ');
assert.equal(nodes[2].textContent, 'GPIO1 signal');
assert.equal(nodes[3].textContent, 'RMS: 10 | Peak: 20 (PCM)');
assert.equal(nodes[4].textContent, 'Settings');
assert.equal(window.confirm('Delete the file Settings.wav? This cannot be undone.'), 'Delete the file Settings.wav? This cannot be undone.');
nodes[1].textContent = 'Stop recording';
window.RR_I18N.applyLanguage('en');
assert.equal(nodes[1].textContent, 'Stop recording');
window.RR_I18N.applyLanguage('hu');
assert.equal(nodes[0].textContent, catalogs.hu.web.Settings);
assert.equal(nodes[1].textContent, catalogs.hu.web['Stop recording']);
assert.equal(nodes[2].textContent, catalogs.hu.web['GPIO{number} signal'].replace('{number}', '1'));
assert.equal(window.RR_I18N.translate('3 WAVs, '), '3 WAV, ');
assert.equal(window.RR_I18N.translate('Folder ('), 'Mappa (');
assert.equal(window.RR_I18N.translate('start'), 'eleje');
assert.equal(window.RR_I18N.translate('Every 15 min'), '15 percenként');
assert.equal(window.RR_I18N.translate('Next: 2026. 11. 08.'), 'Következő: 2026. 11. 08.');
assert.equal(window.RR_I18N.translate('0 waiting'), '0 várakozik');
assert.equal(window.RR_I18N.translate('/Archive'), '/Archive');
assert.equal(window.RR_I18N.applyLanguage('invalid'), false);
assert.equal(document.documentElement.lang, 'hu');
assert.equal(window.RR_I18N.translate('Preflight: 64 / 6464 bytes'),
             catalogs.hu.web['Preflight: {value} bytes'].replace('{value}', '64 / 6464'));
"""
        result = subprocess.run([node, "-"], input=script, encoding="utf-8", text=True,
                                capture_output=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_translation_and_persistent_language_setter(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            self.skipTest("Host g++ required for native language regression")
        source = (ROOT / "src/app_control.cpp").read_text(encoding="utf-8")
        functions = "const char *appLanguage()" + source.split("const char *appLanguage()", 1)[1].split("bool appSetScreenOffMinutes", 1)[0]
        self.assertIn('getString(kLanguageKey, "hu")', source)
        harness = r"""
#include <cassert>
#include <string>
#include "language.h"
using String = std::string;
static String selectedLanguage = "hu";
static const char *kLanguageKey = "language";
static const char *kSettingsNamespace = "settings";
static bool canOpen = true, canWrite = true;
static int writes = 0;
struct Preferences {
  bool begin(const char *, bool) { return canOpen; }
  size_t putString(const char *, const String &value) { ++writes; return canWrite ? value.length() : 0; }
  void end() {}
};
""" + '\nconst std::string huSettings = ' + json.dumps(load_languages()["hu"]["oled"]["Settings"]) + ';\n' + \
            'const std::string huTime = ' + json.dumps(load_languages()["hu"]["oled"]["Time"]) + ';\n' + functions + r"""
int main() {
  assert(std::string(appLanguage()) == "hu");
    assert(std::string(appNextLanguage()) == "en");
    assert(std::string(appText("Settings")) == huSettings);
    assert(std::string(appText("Time")) == huTime);
  assert(!appSetLanguage("xx"));
  assert(writes == 0);
  assert(appSetLanguage("en"));
    assert(std::string(appNextLanguage()) == "hu");
    assert(std::string(appText("Settings")) == "Settings");
  assert(std::string(appText("unknown filename.wav")) == "unknown filename.wav");
  assert(writes == 1);
  assert(appSetLanguage("en"));
  assert(writes == 1);
  canWrite = false;
  assert(!appSetLanguage("hu"));
  assert(std::string(appLanguage()) == "en");
  canOpen = false;
  assert(!appSetLanguage("hu"));
  assert(std::string(appLanguage()) == "en");
  canOpen = canWrite = true;
  assert(appSetLanguage("hu"));
    assert(std::string(appText("Settings")) == huSettings);
}
"""
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with TemporaryDirectory() as folder:
            directory = Path(folder)
            generate_languages(directory / "language.generated.h")
            (directory / "app_control.h").write_text('const char *appLanguage();\n', encoding="utf-8")
            (directory / "language-test.cpp").write_text(harness, encoding="utf-8")
            executable = directory / ("language-test.exe" if os.name == "nt" else "language-test")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(directory),
                                       "-I", str(ROOT / "include"), str(directory / "language-test.cpp"),
                                       str(ROOT / "src/language.cpp"), "-o", str(executable)],
                                      capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)