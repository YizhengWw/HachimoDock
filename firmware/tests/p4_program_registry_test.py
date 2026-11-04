"""Default-off fixed native parent, real PROGRAM broker, no device/audio/IO."""
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_registry_exact_owners_aggregate_turns_pins_checkpoints_and_real_revocation(tmp_path):
    check_registry(tmp_path,False)

def test_registry_phase_observation_preserves_all_original_ownership_and_revocation_vectors(tmp_path):
    check_registry(tmp_path,True)

def check_registry(tmp_path,phase):
    binary=tmp_path/'registry'
    sources=('resources','resources_device','component_program','component_program_session','program_registry',
      'program_core','program_image','program_runtime','program_host','program_host_device',
      'program_group','program_scheduler','program_timers','event_bus')
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined',
      '-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_COMPONENT_BACKGROUND_ENABLED=1',
      f'-DPET_PROGRAM_PHASE_TRACE={int(phase)}',
      '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(vendor),str(ROOT/'tests/p4_program_registry_test.c'),
      *[str(ROOT/'main'/f'pet_p4_{n}.c') for n in sources],str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
    assert result.returncode==0 and not result.stderr,result.stderr

def test_registry_is_inert_in_ordinary_profiles_and_cannot_enable_without_real_program_backend(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(ROOT/'main/pet_p4_program_registry.c'),'-o',str(obj)],check=True)
    symbols=subprocess.run(['nm',str(obj)],capture_output=True,text=True,check=True).stdout
    assert 'pet_program_registry_' not in symbols
    bad=subprocess.run(['cc','-std=c11','-DPET_COMPONENT_BACKGROUND_ENABLED=1','-I',str(ROOT/'main'),'-c',
      str(ROOT/'main/pet_p4_program_registry.c'),'-o',str(obj)],capture_output=True,text=True)
    assert bad.returncode!=0 and 'Background parent requires' in bad.stderr
