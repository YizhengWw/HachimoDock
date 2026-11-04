"""Actual broker linearization with synthetic admission; never hardware/audio."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_device_commit_and_realtime_transition_are_one_atomic_boundary(tmp_path):
    binary=tmp_path/'publication'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pedantic',
        '-fsanitize=address,undefined','-pthread','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
        '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(ROOT/'tests/vendor/cjson'),
        *[str(ROOT/'main'/('pet_p4_program_'+n+'.c')) for n in ('core','image','runtime','host','host_device')],
        str(ROOT/'main/pet_p4_resources.c'),str(ROOT/'main/pet_p4_resources_device.c'),
        str(ROOT/'tests/p4_program_publication_test.c'),'-o',str(binary)],check=True,timeout=30)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20)
    assert result.returncode==0 and not result.stderr,result.stderr
    assert '1000 concurrent publication boundaries' in result.stdout

def test_device_critical_section_has_only_exact_fixed_copy_not_vm_or_callbacks():
    source=(ROOT/'main/pet_p4_resources_device.c').read_text().split('static bool program_state_publish(',1)[1]
    locked=source.split('portENTER_CRITICAL(&resource_mux);',1)[1].split('portEXIT_CRITICAL(&resource_mux);',1)[0]
    assert 'pet_resources_current(&broker,lease)' in locked and 'memcpy(target,candidate,sizeof(*target))' in locked
    for forbidden in ('pet_program_step','pet_program_commit','audio_','publish(', 'malloc(', 'vTaskDelay', 'fopen(', 'while('):assert forbidden not in locked
    assert 'sizeof(pet_program_state_t)<=512' in source
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    private='env:esp32_p4_program_candidate'
    background='env:esp32_p4_background_candidate'
    parent='env:esp32_p4_program_parent_candidate'
    acceptance='env:esp32_p4_program_parent_acceptance'
    mixed='env:esp32_p4_framework_mixed_acceptance'
    public='env:esp32_p4_program_public_candidate'  # explicit public profile table; see p4_public_program_profile_test
    for name in config.sections():
        if name in (private,background,parent,acceptance,mixed,public):continue
        assert 'PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1' not in config[name].get('build_flags','')
        assert private not in config[name].get('extends','')
        assert background not in config[name].get('extends','')
        assert parent not in config[name].get('extends','')
        assert acceptance not in config[name].get('extends','')
        assert 'PET_COMPONENT_BACKGROUND_ENABLED=1' not in config[name].get('build_flags','')
    # Actual publication remains broker-checked in the single-lease private
    # experiment; no other target inherits its physical admission policy.
    assert config[private]['extends']=='env:esp32_p4_canvas_candidate'
    assert config[background]['extends']==private
    assert config[parent]['extends']==background
    assert 'PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1' in config[parent]['build_flags']
    assert config[acceptance]['extends']==parent
    assert 'PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1' in config[acceptance]['build_flags']
    assert config[mixed]['extends']==acceptance
    assert 'PET_P4_UART_PACED_BYTES=256' in config[mixed]['build_flags']

def test_fixed_publication_probe_actual_broker_without_opening_program_quota(tmp_path):
    binary=tmp_path/'publication-probe';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-error=deprecated-declarations','-pedantic',
        '-fsanitize=address,undefined','-pthread','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(vendor),
        str(ROOT/'main/pet_p4_program_core.c'),str(ROOT/'main/pet_p4_program_publication_probe.c'),
        str(ROOT/'main/pet_p4_resources.c'),str(ROOT/'main/pet_p4_resources_device.c'),str(vendor/'cJSON.c'),
        str(ROOT/'tests/p4_program_publication_probe_test.c'),'-o',str(binary)],check=True,timeout=30)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    assert result.returncode==0 and not result.stderr,result.stderr
    assert 'no program grant' in result.stdout
