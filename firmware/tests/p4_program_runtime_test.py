"""Default-off P4P1 staged loader + pure-compute cooperative runtime.

No USB, firmware build, package deployment, cloud, filesystem mutation, or audio.
Host speed is not evidence that the P4 wall-time gate passes.
"""
from pathlib import Path
import json
import os

import pytest

from p4_program_test import WARNINGS, FREESTANDING, INCLUDES, run, wasm_tools, p4_compiler

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "main" / name for name in (
    "pet_p4_program_core.c", "pet_p4_program_image.c", "pet_p4_program_runtime.c")]
TEST = ROOT / "tests/p4_program_runtime_test.c"
CASES = ["loader_valid", "loader_invalid", "queue_transaction", "all_quanta_owned_code",
         "cancel_and_stale", "instruction_revocation_reentry", "trap_no_replay",
         "snapshot_rollback", "commit_boundary", "argument_bounds", "exhausted_revision_restore", "copied_instance_rejected"]


@pytest.fixture(scope="module")
def native_runtime(tmp_path_factory):
    target = tmp_path_factory.mktemp("program-runtime") / "runtime-tests"
    run([os.environ.get("CC", "cc"), *WARNINGS, "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", *INCLUDES, *SOURCES, TEST, "-o", target])
    return target


@pytest.mark.parametrize("case", range(len(CASES)), ids=CASES)
def test_runtime_native_cases(native_runtime, case):
    result = json.loads(run([native_runtime, case]))
    assert result["ok"]
    assert result["imageBytes"] <= 2080
    assert result["runtimeBytes"] <= 3500


def test_runtime_native_fuzz(native_runtime):
    assert json.loads(run([native_runtime]))["ok"]  # 10,000 malformed/mutated images.


def test_runtime_same_c_wasm(tmp_path):
    tools = wasm_tools()
    if not tools:
        pytest.skip("existing wasm clang/linker/node required; no tools downloaded")
    clang, node, linker = tools
    objects = []
    for source in [*SOURCES, TEST]:
        obj = tmp_path / (source.stem + ".o")
        run([clang, "--target=wasm32", *WARNINGS, "-O2", *FREESTANDING, "-nostdlib",
             "-DPET_PROGRAM_WASM", *INCLUDES, "-c", source, "-o", obj])
        objects.append(obj)
    target = tmp_path / "runtime.wasm"
    flavor = ["-flavor", "wasm"] if Path(linker).name == "rust-lld" else []
    run([linker, *flavor, "--no-entry", "--export=pet_program_runtime_selftest",
         "--export=pet_program_runtime_fuzz", "--initial-memory=131072",
         "--max-memory=131072", "-z", "stack-size=16384", *objects, "-o", target])
    script = """
const fs = require('node:fs');
(async () => {
 const module = await WebAssembly.compile(fs.readFileSync(process.argv[1]));
 if (WebAssembly.Module.imports(module).length) throw Error('unexpected import');
 const {exports: e} = await WebAssembly.instantiate(module);
 const r = {selftest:e.pet_program_runtime_selftest(), fuzz:e.pet_program_runtime_fuzz(10000)};
 if (r.selftest || r.fuzz) throw Error(JSON.stringify(r));
 console.log(JSON.stringify(r));
})().catch(e => { console.error(e); process.exitCode = 1; });
"""
    assert json.loads(run([node, "-e", script, target])) == {"selftest": 0, "fuzz": 0}


def test_runtime_no_ambient_effects_and_still_disabled():
    for path in SOURCES[1:]:
        source = path.read_text()
        for forbidden in ("malloc(", "calloc(", "realloc(", "free(", "fopen(",
                          "printf(", "pet_audio", "vTaskDelay", "esp_", "#include <stdlib"):
            assert forbidden not in source
    limits = (ROOT / "main/pet_p4_resources.c").read_text()
    assert "[PET_RESOURCE_PROGRAM]={0,0}" in limits


def runtime_p4_object_report(tmp_path, compiler):
    """Cross-object only. No IDF/PIO firmware build, link, device, or timing claim."""
    flags = ["-march=rv32imafc_zicsr_zifencei_xesppie", "-mabi=ilp32f"]
    prefix = str(compiler).removesuffix("gcc")
    modules = {}
    for source in SOURCES[1:]:
        obj = tmp_path / (source.stem + ".o")
        run([compiler, *WARNINGS, "-O2", *flags, *FREESTANDING, "-fstack-usage",
             *INCLUDES, "-c", source, "-o", obj])
        sizes = run([prefix + "size", obj]).splitlines()[-1].split()
        assert int(sizes[1]) == int(sizes[2]) == 0  # Caller owns all storage.
        frames = {}
        for line in obj.with_suffix(".su").read_text().splitlines():
            name, amount, kind = line.split("\t")
            assert kind == "static", line
            frames[name.rsplit(":", 1)[1]] = int(amount)
        assert max(frames.values()) <= 256  # Individual frames, not call-chain stack.
        imports = [line.split()[-1] for line in run([prefix + "nm", "-u", obj]).splitlines()]
        allowed = {"pet_program_validate", "pet_program_image_program", "pet_program_init",
                   "pet_program_begin", "pet_program_step", "pet_program_commit_with_publication",
                   "pet_program_output_contract_valid", "pet_program_outputs_fit",
                   "pet_program_state_read"}  # Pure bounded accessor, not an ambient effect.
        allowed.add('pet_program_step_with_checkpoint')  # Same pure core, native scheduling only.
        assert set(imports) <= allowed, imports
        modules[source.stem] = {"textBytes": int(sizes[0]), "framesBytes": frames,
                                "unresolvedCoreFunctions": imports}
    layout = tmp_path / "layout.o"
    run([compiler, *WARNINGS, "-O2", *flags, *FREESTANDING, *INCLUDES, "-c",
         ROOT / "tests/p4_program_runtime_layout.c", "-o", layout])
    sizes = {}
    for line in run([prefix + "nm", "-S", "--defined-only", layout]).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3].startswith("pet_runtime_layout_"):
            sizes[fields[3].removeprefix("pet_runtime_layout_")] = int(fields[1], 16)
    assert sizes["image"] <= 2080 and sizes["runtime"] <= 3500
    return {"modules": modules, "layoutBytes": sizes, "runtimeMeasured": False}


def test_runtime_p4_object_only(tmp_path):
    compiler = p4_compiler()
    if not compiler:
        pytest.skip("existing P4 cross compiler required; no tools downloaded")
    runtime_p4_object_report(tmp_path, compiler)
