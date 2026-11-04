"""Explicit monotonic event scheduling, same C on native/WASM. No real timers/IO."""
import json
import os
from pathlib import Path

import pytest

from p4_program_test import ROOT, WARNINGS, INCLUDES, FREESTANDING, run, wasm_tools, p4_compiler

CORE = ROOT / 'main/pet_p4_program_timers.c'
TEST = ROOT / 'tests/p4_program_timers_test.c'


def test_timer_native_sanitizers(tmp_path):
    executable = tmp_path / 'timers'
    run([os.environ.get('CC', 'cc'), *WARNINGS, '-O1', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', *INCLUDES, CORE, TEST, '-o', executable])
    result = json.loads(run([executable]))
    assert result['result'] == 0 and result['trials'] == 20000 and result['timerBytes'] <= 400


def test_timer_real_host_admission_cancel_and_no_implicit_restart(tmp_path):
    executable = tmp_path / 'host-timers'
    sources = [ROOT / 'main' / f'pet_p4_program_{name}.c' for name in ('core', 'image', 'runtime', 'host', 'timers')]
    run([os.environ.get('CC', 'cc'), *WARNINGS, '-O1', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', *INCLUDES, *sources, ROOT / 'tests/p4_program_timers_host_test.c', '-o', executable])
    assert run([executable]) == 'timer host lifetime passed'


def test_timer_same_c_fixed_wasm(tmp_path):
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
    wasm = tmp_path / 'timers.wasm'
    flavor = ['-flavor', 'wasm'] if Path(linker).name == 'rust-lld' else []
    run([linker, *flavor, '--no-entry', '--export=pet_program_timers_selftest',
         '--initial-memory=131072', '--max-memory=131072', '-z', 'stack-size=16384', *objects, '-o', wasm])
    assert run([node, '-e', "const fs=require('node:fs'),m=new WebAssembly.Module(fs.readFileSync(process.argv[1]));if(WebAssembly.Module.imports(m).length)throw Error('imports');console.log(new WebAssembly.Instance(m,{}).exports.pet_program_timers_selftest());", wasm]) == '0'


def test_timer_p4_object_bounded_no_io_or_scheduler_dependency(tmp_path):
    compiler = p4_compiler()
    if not compiler:
        pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj = tmp_path / 'timers.o'
    run([compiler, *WARNINGS, '-Os', '-march=rv32imafc_zicsr_zifencei_xesppie', '-mabi=ilp32f',
         *FREESTANDING, '-fstack-usage', *INCLUDES, '-c', CORE, '-o', obj])
    prefix = str(compiler).removesuffix('gcc')
    imports = {line.split()[-1] for line in run([prefix+'nm', '-u', obj]).splitlines()}
    assert imports <= {'__udivdi3', '__umoddi3', '__udivmoddi4'}
    for line in obj.with_suffix('.su').read_text().splitlines():
        _, amount, kind = line.split('\t')
        assert kind == 'static' and int(amount) <= 256
    for forbidden in ('malloc(', 'fopen(', 'vTask', 'esp_', 'pet_audio_', '#include <stdlib'):
        assert forbidden not in CORE.read_text()
