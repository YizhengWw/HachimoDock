#!/usr/bin/env python3
"""
[Input] Canonical built-in component manifests, runtime/button JSON, and bounded PNG sprites under pc/.
[Output] Deterministic firmware-embedded component bundle with compiled P4 sprites, or a stale-bundle check.
[Pos] ESP32-P4 build-time generator used by main/CMakeLists.txt.
[Sync] If this file changes, update firmware/.folder.md and protocol.md.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
from pathlib import Path

from p4_component_sprites import compile_component_sprites, sprite_pack_checksum


RUNTIME_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = RUNTIME_ROOT.parent
COMPONENT_ROOT = REPOSITORY_ROOT / "pc" / "builtin-clawpkgs"
BUNDLE_PATH = RUNTIME_ROOT / "main" / "pet_p4_builtin_components.json"
DATA_SOURCE_PATH = RUNTIME_ROOT / "main" / "pet_p4_builtin_components_data.c"
BUILTIN_IDS = (
    "two-key-pong",
    "bloomfrog_companion",
    "flappy-bird",
    "block-combo",
    "snake-turn",
    "tomato-clock",
    "drink-reminder",
    "token-usage",
)


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def render_bundle() -> str:
    components = []
    for component_id in BUILTIN_IDS:
        package = COMPONENT_ROOT / component_id
        manifest = read_json(package / "component.json")
        if manifest.get("id") != component_id:
            raise ValueError(f"component id mismatch: {package}")
        widget = read_json(package / "runtime" / "widget.json")
        sprites = compile_component_sprites(package, widget)
        components.append(
            {
                "id": component_id,
                "version": str(manifest.get("version", "")),
                "widget": widget,
                "buttons": read_json(package / "buttons.json"),
                "spritesChecksum": f"{sprite_pack_checksum(sprites):08x}",
                "spriteFiles": [
                    {
                        "id": sprite["id"],
                        "data": base64.b64encode(sprite["data"]).decode("ascii"),
                    }
                    for sprite in sprites
                ],
            }
        )
    return json.dumps(
        {"version": 1, "components": components},
        ensure_ascii=False,
        separators=(",", ":"),
    ) + "\n"


def render_data_source(bundle: str) -> str:
    encoded = bundle.encode("utf-8") + b"\0"
    rows = []
    for offset in range(0, len(encoded), 16):
        chunk = encoded[offset : offset + 16]
        rows.append("  " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(
        [
            "/*",
            " * [Input] Generated deterministic JSON from pc/builtin-clawpkgs.",
            " * [Output] Null-terminated firmware-resident eight-component bundle with P4 sprites.",
            " * [Pos] Generated data source linked into the ESP32-P4 application image.",
            " * [Sync] Regenerate with tools/build_builtin_bundle.py; do not edit manually.",
            " */",
            "",
            "const unsigned char pet_p4_builtin_components_json[] = {",
            *rows,
            "};",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Build firmware built-in component bundle")
    parser.add_argument("--check", action="store_true", help="fail if the tracked bundle is stale")
    args = parser.parse_args()
    rendered_bundle = render_bundle()
    rendered_source = render_data_source(rendered_bundle)
    if args.check:
        current_bundle = BUNDLE_PATH.read_text(encoding="utf-8") if BUNDLE_PATH.is_file() else ""
        current_source = (
            DATA_SOURCE_PATH.read_text(encoding="utf-8") if DATA_SOURCE_PATH.is_file() else ""
        )
        if current_bundle != rendered_bundle or current_source != rendered_source:
            print(f"stale firmware built-in bundle: {BUNDLE_PATH}", file=sys.stderr)
            return 1
        return 0

    BUNDLE_PATH.parent.mkdir(parents=True, exist_ok=True)
    bundle_temporary = BUNDLE_PATH.with_suffix(".json.tmp")
    source_temporary = DATA_SOURCE_PATH.with_suffix(".c.tmp")
    bundle_temporary.write_text(rendered_bundle, encoding="utf-8")
    source_temporary.write_text(rendered_source, encoding="utf-8")
    os.replace(bundle_temporary, BUNDLE_PATH)
    os.replace(source_temporary, DATA_SOURCE_PATH)
    print(BUNDLE_PATH)
    print(DATA_SOURCE_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
