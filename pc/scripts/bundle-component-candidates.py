"""Validate and package an isolated candidate set. Does not install or publish online."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("packages", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--capabilities", required=True, type=Path)
    args = parser.parse_args()
    source, output = args.packages.resolve(), args.output.resolve()
    if output.exists() or output == source or source in output.parents:
        raise ValueError("Use a new isolated output directory, outside the packages")
    output.mkdir(parents=True)
    scripts = Path(__file__).resolve().parents[1] / "skills/petui/scripts"
    results = []
    for package in sorted(source.iterdir()):
        if not package.is_dir():
            continue
        manifest = json.loads((package / "component.json").read_text())
        result = subprocess.run([
            sys.executable, str(scripts / "publish_generated_widget.py"), str(package),
            "--pet-home", str(output / "isolated-publication"),
            "--source-agent", "codex-offline-candidate-validation",
            "--capabilities", str(args.capabilities.resolve()),
        ], capture_output=True, text=True, check=True)
        publication = json.loads(result.stdout)
        archive = output / f"{manifest['id']}.clawpkg"
        files = []
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for file in sorted(package.rglob("*")):
                if file.is_symlink():
                    raise ValueError("Symlinks are not distributable")
                if not file.is_file():
                    continue
                name = file.relative_to(package).as_posix()
                data = file.read_bytes()
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                z.writestr(info, data, compresslevel=9)
                files.append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        results.append({
            "id": manifest["id"], "name": manifest["name"], "version": manifest["version"],
            "archive": archive.name, "bytes": archive.stat().st_size,
            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "files": files, "isolatedPublication": publication,
        })
    report = {
        "status": "offline-candidates-not-installed",
        "capabilityEvidence": "offline fixture, not a connected-device handshake",
        "hardware": "not-tested", "onlinePublication": False,
        "components": results,
    }
    (output / "asset-index.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"{len(results)} candidate packages; installed user library and cloud unchanged")

if __name__ == "__main__":
    main()
