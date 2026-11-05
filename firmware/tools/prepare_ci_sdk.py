"""Prepare real, separate v1/v3 SDK contexts for the full host test suite.

Uses the project's locked PlatformIO environments. No device discovery, upload,
fixture substitutions, ignored test selection or runtime acceptance claims.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--v3-core", required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    cores = {"v1": Path.home() / ".platformio", "v3": Path(args.v3_core).resolve()}
    if cores["v1"].resolve() == cores["v3"]:
        raise SystemExit("v1 and v3 require separate SDK directories")
    exports = {}
    for family, core in cores.items():
        target = "esp32_p4_program_public_candidate" + ("_v3" if family == "v3" else "")
        env = {**os.environ, "PLATFORMIO_CORE_DIR": str(core)}
        subprocess.run([sys.executable, "-m", "platformio", "run", "--project-dir", str(project),
                        "--environment", target, "--target", "compiledb", "--disable-auto-clean"], env=env, check=True)
        database = project / ".pio/build" / target / "compile_commands.json"
        entries = json.loads(database.read_text())
        source = project / "main/pet_p4_storage_io_trace.c"
        matches = [entry for entry in entries if Path(entry["file"]).resolve() == source]
        if len(matches) != 1:
            raise SystemExit("ESP-IDF database must contain the production trace source exactly once")
        cc = core / "packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-gcc"
        sdk = core / "packages/framework-espidf"
        if not cc.is_file() or not (sdk / "tools/cmake/version.cmake").is_file():
            raise SystemExit("Prepared SDK/compiler is incomplete")
        exports[f"PET_P4_{family.upper()}_COMPILE_COMMANDS"] = str(database)
        exports[f"PET_{family.upper()}_SDK_ROOT"] = str(sdk)
        exports[f"PET_{family.upper()}_SDK_CC"] = str(cc)
        if family == "v3":
            exports["PET_P4_V3_IDF_PATH"] = str(sdk)
    for family in cores:
        database = Path(exports[f"PET_P4_{family.upper()}_COMPILE_COMMANDS"])
        if not database.is_file() or not json.loads(database.read_text()):
            raise SystemExit("Both SDK compilation databases must survive preparation")
    github_env = os.environ.get("GITHUB_ENV")
    if not github_env:
        raise SystemExit("GITHUB_ENV is required; SDK inputs prepared but test environment not exported")
    with open(github_env, "a", encoding="utf-8") as stream:
        for name, value in exports.items():
            if "\n" in value or "\r" in value:
                raise SystemExit("Invalid SDK path")
            stream.write(f"{name}={value}\n")
    print("Prepared and exported both real SDK compilation contexts.")


if __name__ == "__main__":
    main()
