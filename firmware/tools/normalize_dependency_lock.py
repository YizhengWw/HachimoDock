"""Restore relocatable local-driver paths after an ESP-IDF build; never modify dependency versions."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
lock = root / "dependencies.lock"
text = lock.read_text(encoding="utf-8")
known = {p.name for p in (root / "components").iterdir() if p.is_dir()}

def normalize(match):
    prefix, value = match.groups()
    value = value.strip().strip('"').replace("\\", "/")
    component = value.rsplit("/", 1)[-1]
    if component not in known or "/components/" not in value:
        raise SystemExit("Unexpected local dependency path; review before publishing")
    return prefix + "$PET_P4_PROJECT_DIR/components/" + component

normalized = re.sub(r"(?m)^(\s+path: )([^\n]+)$", normalize, text)
if normalized != text:
    lock.write_text(normalized, encoding="utf-8")
print("Local dependency paths are portable")
