"""Bundle editable web sources into self-contained firmware pages."""

import argparse
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web"
PAGES = {"index": "kPage", "files": "kFilesPage", "settings": "kSettingsPage"}


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
    return document


def generate_header(destination):
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