"""Bundle editable web sources into self-contained firmware pages."""

import argparse
import json
import re
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web"
PAGES = {"index": "kPage", "files": "kFilesPage", "settings": "kSettingsPage", "test": "kTestPage", "radio": "kRadioPage", "dac_test": "kDacTestPage"}


def unique_language_object(pairs):
    values = {}
    for key, value in pairs:
        if key in values:
            raise ValueError("Duplicate language catalog key: " + key)
        values[key] = value
    return values


def load_languages():
    languages = {path.stem: json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_language_object)
                 for path in sorted((ROOT / "locales").glob("*.json"))}
    if not {"hu", "en"} <= languages.keys():
        raise ValueError("Hungarian and English language catalogs are required")
    for section in ("oled", "web"):
        keys = set(languages["en"][section])
        if any(key != value for key, value in languages["en"][section].items()):
            raise ValueError("English catalog keys must match English source text")
        for code, catalog in languages.items():
            if set(catalog[section]) != keys or not all(isinstance(value, str) for value in catalog[section].values()):
                raise ValueError("Incomplete language catalog: " + code + "/" + section)
            if not re.fullmatch(r"[a-z]{2}(?:-[A-Z]{2})?", code):
                raise ValueError("Invalid language code: " + code)
            for key, value in catalog[section].items():
                placeholders = r"%[a-z]" if section == "oled" else r"\{\w+\}"
                if sorted(re.findall(placeholders, key)) != sorted(re.findall(placeholders, value)):
                    raise ValueError("Translation placeholders differ: " + code + "/" + key)
    return languages


def generate_languages(destination):
    languages = load_languages()
    lines = ["#pragma once", "#include <cstddef>",
             "struct LanguageEntry { const char *original; const char *translated; };",
             "struct LanguageCatalog { const char *code; const char *name; const LanguageEntry *entries; size_t count; };"]
    for code, catalog in languages.items():
        identifier = code.replace("-", "_")
        lines.append("static const LanguageEntry kLanguage_" + identifier + "[] = {")
        for key, value in sorted(catalog["oled"].items()):
            lines.append("  {" + json.dumps(key, ensure_ascii=False) + ", " + json.dumps(value, ensure_ascii=False) + "},")
        lines.append("};")
    lines.append("static const LanguageCatalog kLanguages[] = {")
    for code in languages:
        identifier = code.replace("-", "_")
        lines.append('  {"' + code + '", ' + json.dumps(languages[code]["name"], ensure_ascii=False) + ', kLanguage_' + identifier + ", sizeof(kLanguage_" + identifier + ") / sizeof(LanguageEntry)},")
    lines.append("};")
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join(lines) + "\n"
    if not destination.exists() or destination.read_text(encoding="utf-8") != content:
        destination.write_text(content, encoding="utf-8", newline="\n")


class AssetReferences(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.references = []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == "link" and attributes.get("rel") == "stylesheet":
            self.references.append(("style", self.get_starttag_text(), attributes["href"]))
        elif tag == "script" and "src" in attributes:
            self.references.append(("script", self.get_starttag_text(), attributes["src"]))


def localize_document(document):
    catalogs = {code: {"name": catalog["name"], "web": catalog["web"]} for code, catalog in load_languages().items()}
    data = json.dumps(catalogs, ensure_ascii=False).replace("<", "\\u003c")
    script = (WEB_ROOT / "i18n.js").read_text(encoding="utf-8")
    return document.replace("</head>", '<script>window.RR_LANGUAGES = ' + data + ";\n" + script + "</script>\n</head>")


def render_page(name, web_root=WEB_ROOT):
    web_root = Path(web_root).resolve()
    document = (web_root / (name + ".html")).read_text(encoding="utf-8")
    parser = AssetReferences()
    parser.feed(document)
    for kind, original, reference in parser.references:
        asset = (web_root / reference.lstrip("/")).resolve()
        if not asset.is_relative_to(web_root):
            raise ValueError("Asset escapes web source directory: " + reference)
        content = asset.read_text(encoding="utf-8")
        if kind == "style":
            document = document.replace(original, "<style>\n" + content + "</style>")
        else:
            document = document.replace(original + "</script>", "<script>\n" + content + "</script>")
    if web_root == WEB_ROOT.resolve():
        document = localize_document(document)
    return document


def generate_header(destination):
    generate_languages(Path(destination).parent / "language.generated.h")
    definitions = []
    for name, symbol in PAGES.items():
        document = render_page(name)
        delimiter = "RR_" + name.upper()
        if ')' + delimiter + '"' in document:
            raise ValueError("C++ raw string delimiter occurs in " + name)
        definitions.append('const char ' + symbol + '[] PROGMEM = R"' + delimiter + '(' +
                           document + ')' + delimiter + '";\n')
    content = "\n".join(definitions)
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_text(encoding="utf-8") != content:
        destination.write_text(content, encoding="utf-8", newline="\n")
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".pio/generated/web_pages.generated.h")
    args = parser.parse_args()
    print(generate_header(args.output))


if __name__ == "__main__":
    main()