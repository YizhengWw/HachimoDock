"""Record native PC encoder outputs with source/spec/content fingerprints.

Run only after surface_image::tests::optimized_package_images_fit_device_budget.
This imports build-owned resources, never a user's installed component library.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("encoded_root", type=Path)
    parser.add_argument("ids", nargs="+")
    args = parser.parse_args()
    for component_id in args.ids:
        if component_id not in {p.name for p in (ROOT / "pc/builtin-clawpkgs").iterdir() if p.is_dir()}:
            raise ValueError("Unknown built-in component")
        package = ROOT / "pc/builtin-clawpkgs" / component_id
        widget = json.loads((package / "runtime/widget.json").read_text())
        output = ROOT / "firmware/assets/builtin-surfaces" / component_id
        output.mkdir(parents=True, exist_ok=True)
        records = {}
        for spec in widget["surface"]["images"]:
            ident = spec["id"]
            data = (args.encoded_root / component_id / (ident + ".p4i")).read_bytes()
            if data[:4] not in (b"P4J1", b"P4I1", b"P4I2") or len(data) > 100000:
                raise ValueError("Invalid native image")
            (output / (ident + ".p4i")).write_bytes(data)
            records[ident] = {
                "source": hashlib.sha256((package / spec["asset"]).read_bytes()).hexdigest(),
                "spec": hashlib.sha256(json.dumps(spec, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
                "encoded": hashlib.sha256(data).hexdigest(),
            }
        (output / "index.json").write_text(json.dumps(records, indent=2) + "\n")
        print(f"{component_id}: {len(records)} native images fingerprinted")


if __name__ == "__main__":
    main()
