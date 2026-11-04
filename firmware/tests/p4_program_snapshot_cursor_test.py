"""Focused native C capture correctness; no firmware/device/wall-time claim."""
from pathlib import Path
import subprocess
import pytest
ROOT=Path(__file__).resolve().parents[1]
BASE=['-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer']
def flags(wide):
    result=['-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_COMPONENT_BACKGROUND_ENABLED=1']
    if wide:result+=['-DPET_P4_PROGRAM_V2=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1','-DPET_P4_PUBLIC_PROGRAM_PROFILE=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1']
    return result

def execute(command,binary):
    subprocess.run(command,check=True,capture_output=True,text=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
    assert result.returncode==0 and not result.stderr,result.stderr or result.stdout
    assert result.stdout.startswith('PASS ')

@pytest.mark.parametrize('wide',[False,True],ids=['v1-64cells','v2-public-256cells'])
def test_cursor_real_changed_state_and_generation(tmp_path,wide):
    binary=tmp_path/'cursor'
    execute(['cc',*BASE,*flags(wide),'-I',str(ROOT/'main'),*[str(ROOT/'main'/f'pet_p4_program_{n}.c') for n in ['core','image','runtime']],str(ROOT/'tests/p4_program_snapshot_cursor_test.c'),'-o',str(binary)],binary)

@pytest.mark.parametrize('wide',[False,True],ids=['v1','v2-public'])
@pytest.mark.parametrize('phase',[False,True],ids=['phase-off','phase-on'])
def test_capture_real_registry_invalidation_and_retirement(tmp_path,wide,phase):
    binary=tmp_path/'registry-capture';vendor=ROOT/'tests/vendor/cjson'
    sources=['resources','resources_device','component_program','component_program_session','program_registry','program_core','program_image','program_runtime','program_host','program_host_device','program_group','program_scheduler','program_timers','event_bus']
    execute(['cc',*BASE,*flags(wide),f'-DPET_PROGRAM_PHASE_TRACE={int(phase)}','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(vendor),str(ROOT/'tests/p4_program_registry_capture_test.c'),*[str(ROOT/'main'/f'pet_p4_{n}.c') for n in sources],str(vendor/'cJSON.c'),'-lm','-o',str(binary)],binary)
