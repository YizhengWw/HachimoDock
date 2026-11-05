"""Isolated experimental VM: native sanitizer + same-C WASM + optional P4 object.

No package load, USB, firmware build/flash, network or audio. Run with pytest, or
directly with --measure to print a reproducible JSON report. P4_CC may select an
already installed cross compiler; this script never installs one.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "main/pet_p4_program_core.c"
TEST = ROOT / "tests/p4_program_test.c"
WARNINGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
INCLUDES = ["-I", str(ROOT / "main"), "-I", str(ROOT / "tests")]
FREESTANDING = ["-ffreestanding", "-fno-builtin"]


def run(command):
    result = subprocess.run([str(x) for x in command], check=False, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stdout + result.stderr
    # UBSan can continue with exit code zero, and caller-owned sanitizer options
    # can also change error exit codes. Diagnostics are never a successful run.
    assert not any(marker in result.stderr for marker in (
        'runtime error:', 'ERROR: AddressSanitizer', 'SUMMARY: AddressSanitizer',
        'UndefinedBehaviorSanitizer:', 'WARNING: ThreadSanitizer', 'SUMMARY: ThreadSanitizer',
        'ERROR: LeakSanitizer')), 'sanitizer diagnostic despite successful process exit'
    return result.stdout.strip()


def find_tool(name, candidates=()):
    path = shutil.which(name)
    if path:
        return path
    return next((str(p) for p in candidates if p.is_file()), None)


def native(tmp, optimize=False):
    exe = tmp / ("program-fast" if optimize else "program-sanitized")
    options = ["-O2"] if optimize else ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    run([os.environ.get("CC", "cc"), *WARNINGS, *options, *INCLUDES, CORE, TEST, "-o", exe])
    report = json.loads(run([exe]))
    assert report["selftest"] and report["fuzzTrials"] == 50000
    assert report["checksum"] == 1400000
    assert report["instructionBytes"] == 8
    return report


def wasm_tools():
    clang = find_tool("clang")
    node = find_tool("node", [Path("/opt/homebrew/bin/node")])
    # No downloads: use wasm-ld or a linker bundled in an existing Rust toolchain.
    installed_lld = sorted((Path.home() / ".rustup/toolchains").glob("*/lib/rustlib/*/bin/rust-lld"))
    linker = find_tool("wasm-ld") or find_tool("rust-lld", installed_lld)
    if not clang or not node or not linker:
        return None
    if "wasm32" not in run([clang, "--print-targets"]):
        return None
    return clang, node, linker


def wasm(tmp, tools):
    clang, node, linker = tools
    core = tmp / "core-wasm.o"
    test = tmp / "test-wasm.o"
    for source, target in ((CORE, core), (TEST, test)):
        run([clang, "--target=wasm32", *WARNINGS, "-O2", *FREESTANDING, "-nostdlib",
             "-DPET_PROGRAM_WASM", *INCLUDES, "-c", source, "-o", target])
    module = tmp / "program.wasm"
    flavor = ["-flavor", "wasm"] if Path(linker).name == "rust-lld" else []
    run([linker, *flavor, "--no-entry", "--export=pet_program_selftest",
         "--export=pet_program_fuzz", "--export=pet_program_bench",
         "--initial-memory=131072", "--max-memory=131072", "-z", "stack-size=16384",
         core, test, "-o", module])
    script = """
const fs = require('node:fs');
(async () => {
  const bytes = fs.readFileSync(process.argv[1]);
  const module = await WebAssembly.compile(bytes);
  if (WebAssembly.Module.imports(module).length) throw Error('unexpected host import');
  const {exports: e} = await WebAssembly.instantiate(module);
  const result = {selftest: e.pet_program_selftest(), fuzz: e.pet_program_fuzz(50000),
    checksum: e.pet_program_bench(10000), wasmBytes: bytes.length,
    memoryBytes: e.memory.buffer.byteLength};
  if (result.selftest || result.fuzz || result.checksum !== 1400000)
    throw Error(JSON.stringify(result));
  console.log(JSON.stringify(result));
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
    report = json.loads(run([node, "-e", script, module]))
    assert report["memoryBytes"] == 131072  # Harness allocation, NOT core RAM usage.
    return report


def p4_compiler():
    explicit = os.environ.get("P4_CC")
    base=Path.home()/".platformio/packages/toolchain-riscv32-esp"
    return explicit or find_tool("riscv32-esp-elf-gcc", [base/"bin/riscv32-esp-elf-gcc",
        base/"riscv32-esp-elf/bin/riscv32-esp-elf-gcc"])


def p4_object(tmp, compiler):
    obj = tmp / "core-p4.o"
    target_flags = ["-march=rv32imafc_zicsr_zifencei_xesppie", "-mabi=ilp32f"]
    run([compiler, *WARNINGS, "-Os", *target_flags, *FREESTANDING, "-fstack-usage",
         *INCLUDES, "-c", CORE, "-o", obj])
    prefix = str(compiler).removesuffix("gcc")
    size = run([prefix + "size", obj]).splitlines()[-1].split()
    assert not run([prefix + "nm", "-u", obj]), "core unexpectedly needs a runtime library"
    layout = tmp / "layout-p4.o"
    run([compiler, *WARNINGS, "-Os", *target_flags, *FREESTANDING, *INCLUDES,
         "-c", ROOT / "tests/p4_program_layout.c", "-o", layout])
    layout_sizes = {}
    for line in run([prefix + "nm", "-S", "--defined-only", layout]).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3].startswith("pet_program_layout_"):
            layout_sizes[fields[3].removeprefix("pet_program_layout_")] = int(fields[1], 16)
    frames = {}
    for line in obj.with_suffix(".su").read_text().splitlines():
        name, amount, kind = line.split("\t")
        assert kind == "static", line
        frames[name.rsplit(":", 1)[1]] = int(amount)
    report = {"compiler": run([compiler, "--version"]).splitlines()[0], "flags": target_flags,
              "textBytes": int(size[0]), "dataBytes": int(size[1]), "bssBytes": int(size[2]),
              "framesBytes": frames, "layoutBytes": layout_sizes, "runtimeMeasured": False}
    assert report["dataBytes"] == report["bssBytes"] == 0
    assert report["textBytes"] <= 4096  # Research regression ceiling, not an install contract.
    assert frames["pet_program_run"] <= 256
    assert frames["pet_program_step"] <= 256 and frames["pet_program_begin"] <= 128
    assert frames['pet_program_step_with_checkpoint'] <= 256
    return report


def test_program_native_sanitizers(tmp_path):
    native(tmp_path)


def test_program_same_c_wasm(tmp_path):
    tools = wasm_tools()
    if not tools:
        pytest.skip("an already-installed wasm-capable clang, linker and node are required")
    wasm(tmp_path, tools)


def test_program_direct_p4_object_only(tmp_path):
    compiler = p4_compiler()
    if not compiler:
        pytest.skip("an already-installed P4 cross compiler is required; never invoke PlatformIO")
    p4_object(tmp_path, compiler)


def test_program_core_has_no_ambient_effects():
    source = CORE.read_text()
    for forbidden in ("malloc(", "calloc(", "realloc(", "free(", "printf(", "fopen(",
                      "esp_", "vTaskDelay", "pet_audio", "pet_resources", "#include <stdlib"):
        assert forbidden not in source


def test_native_evidence_rejects_sanitizer_reports_even_with_zero_exit():
    import sys
    for diagnostic in ('runtime error: synthetic', 'WARNING: ThreadSanitizer: synthetic',
                       'ERROR: AddressSanitizer: synthetic'):
        with pytest.raises(AssertionError, match='sanitizer diagnostic'):
            run([sys.executable, '-c', 'import sys;sys.stderr.write(sys.argv[1]);print("ok")', diagnostic])
    assert run([sys.executable, '-c', 'import sys;sys.stderr.write("ordinary tool note");print("ok")']) == 'ok'


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--measure", action="store_true", required=True)
    parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pet-program-") as directory:
        tmp = Path(directory)
        report = {"sanitizedNative": native(tmp), "optimizedNative": native(tmp, optimize=True)}
        tools = wasm_tools()
        report["wasm"] = wasm(tmp, tools) if tools else {"unavailable": True}
        compiler = p4_compiler()
        report["p4ObjectOnly"] = p4_object(tmp, compiler) if compiler else {"unavailable": True}
        print(json.dumps(report, indent=2))
