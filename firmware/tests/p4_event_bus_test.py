"""Volatile native event bus only, no device/resource quota or external effects."""
import json
import os
from pathlib import Path

import pytest

from p4_program_test import ROOT, WARNINGS, INCLUDES, FREESTANDING, run, wasm_tools, p4_compiler

CORE = ROOT / 'main/pet_p4_event_bus.c'
TEST = ROOT / 'tests/p4_event_bus_test.c'


def test_event_bus_atomic_queues_lifetimes_and_faults_native_sanitizers(tmp_path):
    executable = tmp_path / 'events'
    run([os.environ.get('CC', 'cc'), *WARNINGS, '-O1', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', *INCLUDES, CORE, TEST, '-o', executable])
    result = json.loads(run([executable]))
    assert result['result'] == 0 and result['trials'] == 30000 and result['busBytes'] <= 1280


def test_event_bus_real_host_backpressure_audio_revoke_and_new_generation(tmp_path):
    executable = tmp_path / 'host-events'
    sources = [ROOT / 'main' / f'pet_p4_program_{name}.c' for name in ('core', 'image', 'runtime', 'host')]
    run([os.environ.get('CC', 'cc'), *WARNINGS, '-O1', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', *INCLUDES, CORE, *sources, ROOT / 'tests/p4_event_bus_host_test.c', '-o', executable])
    assert run([executable]) == 'event bus host lifetime passed'


def test_event_bus_same_c_fixed_wasm(tmp_path):
    tools = wasm_tools()
    if not tools:
        pytest.skip('requires already-installed compiler/linker/node, never downloads')
    clang, node, linker = tools
    objects = []
    for index, source in enumerate([CORE, TEST]):
        obj = tmp_path / f'{index}.o'
        run([clang, '--target=wasm32', *WARNINGS, '-O2', *FREESTANDING, '-nostdlib',
             '-DPET_PROGRAM_WASM', *INCLUDES, '-c', source, '-o', obj])
        objects.append(obj)
    wasm = tmp_path / 'events.wasm'
    flavor = ['-flavor', 'wasm'] if Path(linker).name == 'rust-lld' else []
    run([linker, *flavor, '--no-entry', '--export=pet_event_bus_selftest',
         '--initial-memory=131072', '--max-memory=131072', '-z', 'stack-size=16384', *objects, '-o', wasm])
    assert run([node, '-e', "const fs=require('node:fs'),m=new WebAssembly.Module(fs.readFileSync(process.argv[1]));if(WebAssembly.Module.imports(m).length)throw Error('imports');console.log(new WebAssembly.Instance(m,{}).exports.pet_event_bus_selftest());", wasm]) == '0'


def test_event_bus_p4_object_has_no_io_dependencies_or_dynamic_stack(tmp_path):
    compiler = p4_compiler()
    if not compiler:
        pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj = tmp_path / 'events.o'
    run([compiler, *WARNINGS, '-Os', '-march=rv32imafc_zicsr_zifencei_xesppie', '-mabi=ilp32f',
         *FREESTANDING, '-fstack-usage', *INCLUDES, '-c', CORE, '-o', obj])
    prefix = str(compiler).removesuffix('gcc')
    # The P4 ABI may copy its fixed-size native owner argument by value. Only
    # that pure memory helper is permitted, never providers/IO/RTOS imports.
    imports = {line.split()[-1] for line in run([prefix+'nm', '-u', obj]).splitlines()}
    assert imports <= {'memcpy'}
    for line in obj.with_suffix('.su').read_text().splitlines():
        _, amount, kind = line.split('\t')
        assert kind == 'static' and int(amount) <= 256
