"""Resolve actual private flag composition and compile real broker limits."""
from pathlib import Path
import configparser
import re
import shlex
import subprocess
import pytest
ROOT=Path(__file__).resolve().parents[1]

def _check_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(tmp_path, target):
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    def flags(section,visited=()):
        assert section not in visited
        result=[]
        for line in config[section].get('build_flags','').splitlines():
            line=line.strip()
            if not line:continue
            ref=re.fullmatch(r'\$\{([^}]+)\.build_flags\}',line)
            if ref:result.extend(flags(ref[1],(*visited,section)))
            else:result.extend(shlex.split(line))
        return result
    tokens=flags(target);defines={}
    for token in tokens:
        if token.startswith('-D'):
            key,sep,value=token[2:].partition('=');value=value if sep else '1'
            assert key not in defines or defines[key]==value,'conflicting inherited policy'
            defines[key]=value
    for key in ('PET_COMPONENT_PROGRAM_ENABLED','PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED','PET_COMPONENT_BACKGROUND_ENABLED',
      'PET_COMPONENT_PROGRAM_PARENT_UNIFIED','PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE',
      'PET_P4_STORAGE_IO_TRACE_ENABLE','PET_P4_STORAGE_PAGE_CACHE_ENABLE','PET_P4_STORAGE_PAGE_CACHE_APPLY',
      'PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH','PET_P4_STORAGE_CACHE_SCOPE_ENABLED','PET_P4_SERIAL_CONTROL_INTEGRITY','PET_P4_UART_PACED_REFERENCE'):
        assert defines[key]=='1'
    assert defines['PET_P4_UART_PACED_BYTES']=='256'
    metadata=target=='env:esp32_p4_ota_metadata_probe'
    erase_phase=target=='env:esp32_p4_native_erase_phase_probe' or metadata
    wraps={'-Wl,--wrap=spiffs_api_'+op for op in ('read','write','erase')}
    if erase_phase:
        assert config[target]['extends']==('env:esp32_p4_native_erase_phase_probe' if metadata else 'env:esp32_p4_storage_query_probe')
        assert defines['PET_P4_FLASH_PHASE_TRACE']=='1'
        wraps|={'-Wl,--wrap='+name for name in ('esp_partition_erase_range','esp_flash_erase_region',
            'spi_flash_chip_generic_yield','spi_flash_chip_generic_erase_sector',
            'spi_flash_chip_winbond_erase_sector',
            'spi_flash_chip_generic_wait_idle','spi_flash_check_and_flush_cache')}
    else:assert 'PET_P4_FLASH_PHASE_TRACE' not in defines
    assert defines.get('PET_P4_OTA_METADATA_CACHED','0')==('1' if metadata else '0')
    assert set(t for t in tokens if '--wrap=' in t)==wraps
    assert 'PET_P4_PPA_QUEUE_PROBE' not in defines and 'PET_P4_RENDER_ROTATE_TILE' not in defines
    if target=='env:esp32_p4_framework_mixed_acceptance':
        assert config[target]['extends']=='env:esp32_p4_program_parent_acceptance'
        assert 'PET_P4_QUERY_REPLY_TRACE' not in defines and 'PET_P4_QUERY_REPLY_DEFERRED' not in defines
    else:
        storage_pair=target in ('env:esp32_p4_storage_query_control','env:esp32_p4_storage_query_probe') or erase_phase
        if storage_pair:
            if not erase_phase:assert config[target]['extends']=='env:esp32_p4_watchdog_phase_probe'
            assert defines['PET_P4_RESET_PHASE_TRACE']=='1'
            assert defines.get('PET_P4_DIAGNOSTIC_STORAGE_CACHED','0')==('1' if target.endswith('_probe') else '0')
        elif target=='env:esp32_p4_watchdog_phase_probe':
            assert config[target]['extends']=='env:esp32_p4_query_lock_probe'
            assert defines['PET_P4_RESET_PHASE_TRACE']=='1'
        else:
            assert config[target]['extends']=='env:esp32_p4_framework_mixed_acceptance'
            assert 'PET_P4_RESET_PHASE_TRACE' not in defines
        assert defines['PET_P4_QUERY_REPLY_TRACE']=='1'
        assert defines.get('PET_P4_QUERY_REPLY_DEFERRED','0')==('1' if storage_pair or target.endswith('_probe') else '0')
    assert config['env:esp32_p4_evboard']['custom_p4_uart_baud']=='4000000'
    path=tmp_path/'mixed.c';path.write_text('''
#include <assert.h>
#include "pet_p4_resources.h"
#include "pet_p4_catalog_profile.h"
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_control_integrity.h"
#include "pet_p4_uart_pacing.h"
int main(void){
 assert(pet_resources_limit(PET_RESOURCE_PROGRAM).leases==2);
 assert(pet_resources_limit(PET_RESOURCE_PROGRAM).units==4096);
 assert(!pet_resources_limit(PET_RESOURCE_PERIPHERAL).leases);
 assert(PET_P4_MINIAPP_CATALOG_MAX==33&&PET_P4_UART_PACED_BYTES==256);
 assert(PET_P4_STORAGE_CACHE_SCOPE_ENABLED&&PET_P4_SERIAL_CONTROL_INTEGRITY);
 return 0;
}
''');binary=tmp_path/'mixed'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      *[f'-D{k}={v}' for k,v in defines.items()],'-I',str(ROOT/'main'),str(path),str(ROOT/'main/pet_p4_resources.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(tmp_path):
    # Preserve the original acceptance case identity; children are additional
    # cases, not a rename/removal of the previous mixed-profile gate.
    _check_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(
        tmp_path, 'env:esp32_p4_framework_mixed_acceptance')

@pytest.mark.parametrize('target', ['env:esp32_p4_query_lock_control', 'env:esp32_p4_query_lock_probe', 'env:esp32_p4_watchdog_phase_probe'])
def test_actual_query_lock_children_keep_same_mixed_limits(tmp_path, target):
    _check_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(tmp_path, target)

@pytest.mark.parametrize('target', ['env:esp32_p4_storage_query_control','env:esp32_p4_storage_query_probe'])
def test_actual_storage_query_children_keep_identical_mixed_limits(tmp_path,target):
    _check_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(tmp_path,target)

def test_actual_native_erase_child_adds_only_passive_call_boundaries(tmp_path):
    _check_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(tmp_path,'env:esp32_p4_native_erase_phase_probe')

def test_actual_ota_metadata_child_preserves_limits_and_all_legacy_gates(tmp_path):
    _check_mixed_flags_have_one_policy_no_extra_effect_grants_and_same_bounds(tmp_path,'env:esp32_p4_ota_metadata_probe')
