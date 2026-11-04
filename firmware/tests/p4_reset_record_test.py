"""Passive reset evidence only: pure codec, no firmware/device or sound."""
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]

def test_actual_retained_codec_all_torn_store_boundaries_and_native_sanitizers(tmp_path):
    binary=tmp_path/'record'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fstrict-aliasing',
        '-fsanitize=address,undefined','-DPET_RESET_PHASE_HOST_TEST=1','-DPET_RESET_PHASE_STORE_TEST=1',
        '-I',str(ROOT/'main'),str(ROOT/'main/pet_p4_reset_record.c'),str(ROOT/'tests/p4_reset_record_test.c'),
        '-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
    assert 'torn-store cases passed' in result.stdout

def test_reset_record_default_object_is_empty_and_has_no_effect_imports(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-c','-I',str(ROOT/'main'),
        str(ROOT/'main/pet_p4_reset_record.c'),'-o',str(obj)],check=True)
    result=subprocess.run(['nm','-g',str(obj)],check=True,capture_output=True,text=True)
    assert not result.stdout.strip()
    # Mach-O retains a zero-sized local ltmp0 section anchor even for an empty
    # translation unit. Verify actual allocated bytes, not that debug marker.
    sized=subprocess.run(['size','-m' if sys.platform=='darwin' else '-A',str(obj)],check=True,capture_output=True,text=True).stdout
    if sys.platform=='darwin':
        assert 'Section (__TEXT, __text): 0' in sized
        assert all(line.split(':')[-1].strip()=='0' for line in sized.splitlines() if ':' in line)
    else:
        assert all(int(line.split()[1])==0 for line in sized.splitlines() if line.startswith(('.text ','.data ','.bss ')))
    source=(ROOT/'main/pet_p4_reset_record.c').read_text()
    for effect in ('malloc(', 'calloc(', 'free(', 'printf(', 'fopen(', 'nvs_', 'Semaphore', 'portENTER', 'vTaskDelay', 'esp_timer'):
        assert effect not in source
    assert '#define PET_P4_RESET_PHASE_TRACE 0' in (ROOT/'main/pet_p4_reset_record.h').read_text()

def test_actual_phase_adapter_ownership_and_simulated_boot_boundaries(tmp_path):
    binary=tmp_path/'adapter';vendor=ROOT/'tests/vendor/resetphase'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-fstrict-aliasing',
        '-fsanitize=address,undefined','-DPET_P4_RESET_PHASE_TRACE=1','-I',str(vendor),'-I',str(ROOT/'main'),
        str(ROOT/'tests/p4_reset_phase_test.c'),str(ROOT/'main/pet_p4_reset_record.c'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
    assert 'native phase ownership/nesting/stale/default-context/boot cases passed' in result.stdout

def test_passive_adapter_default_empty_and_only_explicit_private_profile(tmp_path):
    import configparser
    obj=tmp_path/'adapter-off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-c','-I',str(ROOT/'main'),
        str(ROOT/'main/pet_p4_reset_phase.c'),'-o',str(obj)],check=True)
    assert not subprocess.run(['nm','-g',str(obj)],check=True,capture_output=True,text=True).stdout.strip()
    sized=subprocess.run(['size','-m' if sys.platform=='darwin' else '-A',str(obj)],check=True,capture_output=True,text=True).stdout
    if sys.platform=='darwin':assert all(line.split(':')[-1].strip()=='0' for line in sized.splitlines() if ':' in line)
    else:assert all(int(line.split()[1])==0 for line in sized.splitlines() if line.startswith(('.text ','.data ','.bss ')))
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_watchdog_phase_probe':
            assert config[section]['extends']=='env:esp32_p4_query_lock_probe'
            assert flags.count('PET_P4_RESET_PHASE_TRACE=1')==1
        else:assert 'PET_P4_RESET_PHASE_TRACE=1' not in flags
    adapter=(ROOT/'main/pet_p4_reset_phase.c').read_text()
    for effect in ('malloc(', 'calloc(', 'printf(', 'fopen(', 'nvs_', 'Semaphore', 'portENTER', 'vTaskDelay', 'esp_timer', 'task_wdt', 'wdt_hal'):
        assert effect not in adapter
    assert 'esp_cpu_get_cycle_count()' in adapter and 'RTC_NOINIT_ATTR' in adapter
    assert 'ESP_IDF_VERSION_VAL(5,5,1)' in adapter
    # Publication's original IRQ-off block has NO recorder call/RTC work.
    source=(ROOT/'main/pet_p4_resources_device.c').read_text().split('static bool program_state_publish(',1)[1]
    locked=source.split('portENTER_CRITICAL(&resource_mux);',1)[1].split('portEXIT_CRITICAL(&resource_mux);',1)[0]
    assert 'pet_reset_' not in locked and 'PET_RESET_' not in locked

def test_raw_reset_diagnostics_are_fixed_cpu_readonly_and_do_not_change_watchdog_policy():
    source=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    assert 'g_raw_reset_reason[0] = (uint32_t)esp_rom_get_reset_reason(0);' in source
    assert 'g_raw_reset_reason[1] = (uint32_t)esp_rom_get_reset_reason(1);' in source
    assert '"lastResetRawReasonCpu0", g_raw_reset_reason[0]' in source
    assert '"lastResetRawReasonCpu1", g_raw_reset_reason[1]' in source
    assert '"lastValidOnly",true' in source and '"authentication",false' in source
    assert '"cycles",r->cycles' in source and '"actor",actor' in source
    adapter=(ROOT/'main/pet_p4_reset_phase.c').read_text()
    for operation in ('esp_task_wdt_', 'wdt_hal_', 'REG_WRITE', 'REG_SET_BIT', 'vTaskPrioritySet', 'xTaskCreate'):
        assert operation not in adapter
    main=(ROOT/'main/pet_p4_main.c').read_text()
    assert 'pet_reset_phase_boot();' in main
    assert 'configASSERT(phase_bound)' not in main
