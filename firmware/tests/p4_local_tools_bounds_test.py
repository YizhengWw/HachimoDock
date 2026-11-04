"""Offline local deck capacity/exclusion/JSON round-trip boundaries; no SDK/device."""
import os
import shutil
import subprocess
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[2]
@pytest.fixture(scope='module')
def local_bounds_exe(tmp_path_factory):
    work=tmp_path_factory.mktemp('local-mask-bounds')
    compiler=shutil.which('clang') or shutil.which('cc')
    assert compiler,'host C compiler required'
    exe=work/'local-bounds'
    command=[compiler,'-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-DPET_P4_QUOTA_V3=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1','-DFIX_CHECK=1',
        '-I'+str(ROOT/'firmware/main'),'-I'+str(ROOT/'firmware/tests/vendor/cjson'),
        str(ROOT/'firmware/tests/p4_local_tools_bounds_test.c'),str(ROOT/'firmware/main/pet_p4_local_tools.c'),
        str(ROOT/'firmware/tests/vendor/cjson/cJSON.c'),'-lm','-o',str(exe)]
    compiled=subprocess.run(command,capture_output=True,text=True)
    assert compiled.returncode==0,compiled.stdout+compiled.stderr
    return exe
@pytest.mark.parametrize('count',[16,17,24,25,31,32,33,44,45,64])
def test_local_deck_masks_exhaust_restore_reset(local_bounds_exe,count):
    run=subprocess.run([str(local_bounds_exe),str(count)],capture_output=True,text=True,
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'),timeout=10)
    assert run.returncode==0,run.stdout+run.stderr
    assert f'BOUNDARY_OK count={count}' in run.stdout
