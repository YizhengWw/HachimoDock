"""Receiver constraints roll back atomically on native C and the same fixed WASM.

Synthetic, local-only execution; never loads components, credentials or devices.
"""
import json
import os
from pathlib import Path

import pytest

from p4_program_test import ROOT, WARNINGS, INCLUDES, FREESTANDING, run, wasm_tools


SOURCES = [ROOT / "main" / name for name in (
    "pet_p4_program_core.c", "pet_p4_program_image.c", "pet_p4_program_runtime.c")]
TEST = ROOT / "tests/p4_program_output_contract_test.c"


def test_receiver_contract_native_sanitizers(tmp_path):
    executable = tmp_path / "output-contract"
    run([os.environ.get("CC", "cc"), *WARNINGS, "-O1", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", *INCLUDES, *SOURCES, TEST, "-o", executable])
    report = json.loads(run([executable]))
    assert report["result"] == 0 and report["trials"] == 5000
    assert report["runtimeBytes"] <= 3500


def test_receiver_contract_same_c_fixed_wasm(tmp_path):
    tools = wasm_tools()
    if not tools:
        pytest.skip("requires existing wasm compiler, linker and node; never downloads")
    clang, node, linker = tools
    objects = []
    for index, source in enumerate([*SOURCES, TEST]):
        target = tmp_path / f"part-{index}.o"
        run([clang, "--target=wasm32", *WARNINGS, "-O2", *FREESTANDING, "-nostdlib",
             "-DPET_PROGRAM_WASM", *INCLUDES, "-c", source, "-o", target])
        objects.append(target)
    module = tmp_path / "output-contract.wasm"
    flavor = ["-flavor", "wasm"] if Path(linker).name == "rust-lld" else []
    run([linker, *flavor, "--no-entry", "--export=pet_program_output_contract_selftest",
         "--initial-memory=131072", "--max-memory=131072", "-z", "stack-size=16384",
         *objects, "-o", module])
    result = json.loads(run([node, "-e", """
const fs=require('node:fs'),m=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
if(WebAssembly.Module.imports(m).length)throw Error('unexpected imports');
const e=new WebAssembly.Instance(m,{}).exports;
let fixed=false;try{e.memory.grow(1);}catch{fixed=true;}
console.log(JSON.stringify({result:e.pet_program_output_contract_selftest(),bytes:e.memory.buffer.byteLength,fixed}));
""", module]))
    assert result == {"result": 0, "bytes": 131072, "fixed": True}
