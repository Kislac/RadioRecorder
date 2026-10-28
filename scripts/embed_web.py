"""PlatformIO pre-build hook: generate firmware pages from web/ sources."""

import sys
from pathlib import Path

Import("env")

root = Path(env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(root))
from scripts.web_assets import generate_header

generated_dir = Path(env.subst("$PROJECT_DIR")) / ".pio" / "generated"
generate_header(generated_dir / "web_pages.generated.h")
env.Append(CPPPATH=[str(generated_dir)])