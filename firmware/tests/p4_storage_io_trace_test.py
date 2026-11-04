"""Private physical-HAL observation; no filesystem/device/acoustic acceptance."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_compact_page_metadata_preserves_every_aligned_address_and_pin_bound(tmp_path):
    source=tmp_path/'page-entry.c';binary=tmp_path/'page-entry'
    source.write_text(r'''
#include <assert.h>
#include "pet_p4_storage_page_entry.h"
int main(void){
  for(uint32_t page=0;page<=PET_PAGE_ADDRESS_MASK;page++){
    uint32_t address=page<<9;
    page_cache_entry_t entry=page_entry_reserved(address);
    assert(page_entry_address(&entry)==address&&page_entry_pins(&entry)==1&&page_entry_filling(&entry));
    entry.generation=UINT32_MAX;page_entry_complete(&entry);
    assert(entry.generation==UINT32_MAX&&page_entry_address(&entry)==address&&!page_entry_pins(&entry)&&!page_entry_filling(&entry));
  }
  page_cache_entry_t entry=page_entry_reserved(UINT32_C(0xfffffe00));
  entry.generation=7;
  for(unsigned n=1;n<PET_PAGE_PIN_MAX;n++)assert(page_entry_pin(&entry));
  uint32_t full=entry.packed;
  assert(!page_entry_pin(&entry)&&entry.packed==full&&page_entry_filling(&entry));
  entry.generation=0; /* Invalidation may not reset ownership metadata. */
  for(unsigned n=0;n<PET_PAGE_PIN_MAX;n++)assert(page_entry_unpin(&entry));
  assert(!page_entry_unpin(&entry)&&page_entry_address(&entry)==UINT32_C(0xfffffe00)&&page_entry_filling(&entry));
  page_entry_complete(&entry);
  assert(!page_entry_pins(&entry)&&!page_entry_filling(&entry)&&!entry.generation);
  return 0;
}
''')
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)

def test_private_automatic_cache_follows_actual_storage_scope_without_adopting_diagnostics(tmp_path):
    binary=tmp_path/'cache-scope'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1',
      '-DPET_P4_STORAGE_PAGE_CACHE_APPLY=1','-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1','-DPET_P4_STORAGE_CACHE_SCOPE_ENABLED=1',
      '-I',str(ROOT/'main'),str(ROOT/'tests/p4_storage_cache_scope_test.c'),str(ROOT/'main/pet_p4_storage_lane.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
    from p4_render_trace_test import function
    source=(ROOT/'main/pet_p4_miniapp.c').read_text()
    begin=function(source,'storage_scope_begin');finish=function(source,'storage_scope_finish')
    assert begin.index('pet_resources_device_acquire(')<begin.index('storage_cache_scope_begin(')<begin.index('return true;')
    assert finish.index('pet_storage_scope_finish(')<finish.index('storage_cache_scope_finish(')<finish.index('pet_resources_device_finish(')
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section in ('env:esp32_p4_storage_page_parent','env:esp32_p4_framework_mixed_acceptance'):
            assert config[section]['extends']==('env:esp32_p4_storage_page_stride' if section=='env:esp32_p4_storage_page_parent' else 'env:esp32_p4_program_parent_acceptance')
            assert 'PET_P4_STORAGE_CACHE_SCOPE_ENABLED=1' in flags
        else:assert 'PET_P4_STORAGE_CACHE_SCOPE_ENABLED=1' not in flags

def test_private_full_page_cache_preserves_pins_across_interruptible_copies(tmp_path):
    for apply,stride in ((0,0),(1,0),(0,1),(1,1)):
        binary=tmp_path/('page-'+str(apply)+str(stride))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
          '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
          '-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1',f'-DPET_P4_STORAGE_PAGE_CACHE_APPLY={apply}',
          f'-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH={stride}',
          '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),str(ROOT/'tests/p4_storage_page_cache_test.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_private_uniform_byte_cache_faults_coherence_and_window_expiry(tmp_path):
    for apply in (0,1):
        binary=tmp_path/('uniform-'+str(apply))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
          '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
          '-DPET_P4_STORAGE_UNIFORM_CACHE_ENABLE=1',f'-DPET_P4_STORAGE_UNIFORM_CACHE_APPLY={apply}',
          '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),str(ROOT/'tests/p4_storage_uniform_test.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_cache_hit_phase_coarsening_preserves_actual_bytes_pins_errors_and_io(tmp_path):
    for hit_phase in (0,1):
        binary=tmp_path/('phase-hits-'+str(hit_phase))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
          '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
          '-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1','-DPET_P4_STORAGE_PAGE_CACHE_APPLY=1',
          '-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1','-DPET_P4_RESET_PHASE_TRACE=1',
          f'-DPET_P4_RESET_PHASE_CACHE_HITS={hit_phase}',
          '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),
          str(ROOT/'tests/p4_storage_page_cache_test.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_private_uniform_cache_runs_original_sdk_cow_gc_and_recovery(tmp_path):
    sdk=Path.home()/'.platformio/packages/framework-espidf/components/spiffs'
    assert (sdk/'spiffs/src/spiffs_nucleus.c').is_file()
    vendor_common=['cc','-std=c11','-O1','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',
      '-I',str(ROOT/'tests/spiffs_hint_stubs'),'-I',str(sdk/'include'),'-I',str(sdk/'spiffs/src')]
    objects=[]
    for name in ('spiffs_cache','spiffs_check','spiffs_gc','spiffs_hydrogen','spiffs_nucleus'):
        obj=tmp_path/(name+'.o')
        # Actual SDK uses intentionally packed unaligned indexes; same narrow
        # vendor-only alignment exclusion as the existing SPIFFS test harness.
        subprocess.run([*vendor_common,'-fno-sanitize=alignment','-c',str(sdk/'spiffs/src'/(name+'.c')),'-o',str(obj)],check=True)
        objects.append(str(obj))
    results=[]
    for kind,apply,stride,partial,sparse in (('UNIFORM',0,0,0,0),('UNIFORM',1,0,0,0),('PAGE',0,0,0,0),('PAGE',1,0,0,0),('PAGE',0,1,0,0),('PAGE',1,1,0,0),('PAGE',1,1,1,0),('PAGE',1,1,1,1)):
        binary=tmp_path/('sdk-'+kind+str(apply)+str(stride)+str(partial)+str(sparse))
        subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
          '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
          f'-DPET_P4_STORAGE_{kind}_CACHE_ENABLE=1',f'-DPET_P4_STORAGE_{kind}_CACHE_APPLY={apply}',
          f'-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH={stride}',
          f'-DPET_P4_STORAGE_PAGE_CACHE_PARTIAL={partial}',
          f'-DPET_P4_STORAGE_PAGE_CACHE_SPARSE={sparse}',
          '-I',str(sdk/'spiffs/src'),'-I',str(sdk/'include'),'-I',str(ROOT/'tests/storage_io_stubs'),
          '-I',str(ROOT/'tests/spiffs_hint_stubs'),'-I',str(ROOT/'main'),
          str(ROOT/'tests/p4_storage_uniform_sdk_test.c'),*objects,'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],check=True,timeout=30,capture_output=True,text=True)
        import re
        parsed=re.search(r'physicalReads=(\d+) hits=(\d+) hash=([0-9a-f]{16})',result.stdout)
        assert parsed,result.stdout
        results.append((int(parsed[1]),int(parsed[2]),parsed[3]))
        print(result.stdout.strip())
    assert results[0][2]==results[1][2],'exact same final NOR bytes required, not just successful APIs'
    assert results[0][1]==0 and results[1][1]>0 and results[1][0]<results[0][0]
    assert results[2][2]==results[3][2]==results[0][2]
    assert results[2][1]==0 and results[3][1]>0 and results[3][0]<results[2][0]
    assert results[4][2]==results[5][2]==results[0][2]
    assert results[4][1]==0 and results[5][1]>0 and results[5][0]<results[4][0]
    assert results[6][2]==results[5][2] and results[6][0]<=results[5][0]
    assert results[7]==results[6],'representation only: same physical calls, hits and complete final NOR hash'

def test_private_uniform_parent_mounts_only_once_and_never_formats():
    main=(ROOT/'main/pet_p4_main.c').read_text()
    assert main.count('init_storage(')==2 # One definition, one boot call.
    assert main.count('esp_vfs_spiffs_register(')==1 and '.format_if_mount_failed = false' in main
    for path in (ROOT/'main').glob('*.c'):
        text=path.read_text()
        for forbidden in ('esp_vfs_spiffs_unregister(', 'esp_spiffs_format(', 'SPIFFS_unmount(', 'SPIFFS_format('):
            assert forbidden not in text,(path.name,forbidden)

def test_uniform_cache_only_private_same_ram_profiles_and_no_external_effects():
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    variants=('env:esp32_p4_storage_uniform_control','env:esp32_p4_storage_uniform_probe')
    for section in c.sections():
        flags=c[section].get('build_flags','')
        if section in variants:
            assert c[section]['extends']=='env:esp32_p4_storage_io_probe'
            assert 'PET_P4_STORAGE_UNIFORM_CACHE_ENABLE=1' in flags
            assert f'PET_P4_STORAGE_UNIFORM_CACHE_APPLY={int(section==variants[1])}' in flags
        else:assert 'PET_P4_STORAGE_UNIFORM_CACHE_' not in flags
    text=(ROOT/'main/pet_p4_storage_uniform_cache.inc').read_text()
    for forbidden in ('malloc(', 'fopen(', 'ESP_LOG', 'printf(', 'vTaskDelay(', '__real_', 'SPIFFS_remove('):
        assert forbidden not in text

def test_full_page_cache_only_private_same_ram_profiles_and_no_external_effects():
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    variants=('env:esp32_p4_storage_page_control','env:esp32_p4_storage_page_probe')
    for section in c.sections():
        flags=c[section].get('build_flags','')
        if section in variants:
            assert c[section]['extends']=='env:esp32_p4_storage_io_probe'
            assert 'PET_P4_STORAGE_PAGE_CACHE_ENABLE=1' in flags
            assert f'PET_P4_STORAGE_PAGE_CACHE_APPLY={int(section==variants[1])}' in flags
        elif section=='env:esp32_p4_storage_page_stride':
            assert c[section]['extends']=='env:esp32_p4_storage_page_probe'
            assert 'PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1' in flags
        elif section=='env:esp32_p4_framework_mixed_acceptance':
            assert c[section]['extends']=='env:esp32_p4_program_parent_acceptance'
            for flag in ('ENABLE','APPLY','STRIDE_HASH'):assert f'PET_P4_STORAGE_PAGE_CACHE_{flag}=1' in flags
            assert 'PET_P4_STORAGE_CACHE_SCOPE_ENABLED=1' in flags
        elif section=='env:esp32_p4_budget_parent':
            assert c[section]['extends']=='env:esp32_p4_phase_parent'
            assert 'PET_P4_STORAGE_PAGE_CACHE_PARTIAL=1' in flags
            assert 'PET_P4_STORAGE_IO_RETAIN_COMPLETED=1' in flags
            for flag in ('ENABLE','APPLY','STRIDE_HASH'):
                assert f'PET_P4_STORAGE_PAGE_CACHE_{flag}=' not in flags
        elif section=='env:esp32_p4_retained_parent':
            assert c[section]['extends']=='env:esp32_p4_logical_parent'
            assert 'PET_P4_STORAGE_PAGE_CACHE_SPARSE=1' in flags and 'PET_P4_APP_RETAIN_LOGICAL=1' in flags
            for flag in ('ENABLE','APPLY','STRIDE_HASH','PARTIAL'):
                assert f'PET_P4_STORAGE_PAGE_CACHE_{flag}=' not in flags
        else:assert 'PET_P4_STORAGE_PAGE_CACHE_' not in flags
    text=(ROOT/'main/pet_p4_storage_page_cache.inc').read_text()
    for forbidden in ('malloc(', 'fopen(', 'ESP_LOG', 'printf(', 'vTaskDelay(', '__real_', 'SPIFFS_remove('):
        assert forbidden not in text

def test_storage_hal_observer_calls_once_preserves_errno_and_bounds_lifecycle(tmp_path):
    binary=tmp_path/'hal'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
      '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),str(ROOT/'tests/p4_storage_io_trace_test.c'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    assert result.returncode==0 and 'PASS exact HAL once' in result.stdout,result.stderr

def test_storage_hal_observer_default_object_is_empty_and_grants_nothing(tmp_path):
    obj=tmp_path/'closed.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(ROOT/'main/pet_p4_storage_io_trace.c'),'-o',str(obj)],check=True)
    result=subprocess.run(['nm','-g',str(obj)],capture_output=True,text=True,check=True)
    assert '__wrap' not in result.stdout and 'pet_storage_io_' not in result.stdout

def test_storage_hal_rpc_strict_native_adapter_and_exact_numeric_serialization(tmp_path):
    vendor=ROOT/'tests/vendor/cjson'
    for retained in (0,1):
        binary=tmp_path/('rpc-'+str(retained))
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-error=deprecated-declarations',
          '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1',f'-DPET_P4_STORAGE_IO_RETAIN_COMPLETED={retained}',
          '-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(ROOT/'main'),'-I',str(vendor),
          str(ROOT/'main/pet_p4_storage_io_trace_rpc.c'),str(ROOT/'tests/p4_storage_io_rpc_test.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
        assert result.returncode==0 and 'PASS private HAL RPC strict schema' in result.stdout,result.stderr

def test_only_private_profile_wraps_hal_and_normal_parents_remain_closed():
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    private='env:esp32_p4_storage_io_probe'
    assert config[private]['extends']=='env:esp32_p4_canvas_candidate'
    for name in config.sections():
        flags=config[name].get('build_flags','')
        if name in (private,'env:esp32_p4_framework_mixed_acceptance'):
            assert 'PET_P4_STORAGE_IO_TRACE_ENABLE=1' in flags
            for symbol in ('read','write','erase'):assert '--wrap=spiffs_api_'+symbol in flags
            assert 'PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1' not in flags
            if name!=private:assert config[name]['extends']=='env:esp32_p4_program_parent_acceptance'
        else:
            assert 'PET_P4_STORAGE_IO_TRACE_ENABLE=1' not in flags and '--wrap=spiffs_api_read' not in flags

def test_private_hal_and_rpc_static_p4_frames_remain_bounded(tmp_path):
    import pytest
    from p4_program_test import p4_compiler
    compiler=p4_compiler()
    if not compiler:pytest.skip('requires existing P4 compiler, does not download or invoke PlatformIO')
    for file,bound,cache,page in (('pet_p4_storage_io_trace',256,0,0),('pet_p4_storage_io_trace_rpc',1536,0,0),('pet_p4_storage_io_trace',256,1,0),('pet_p4_storage_io_trace',768,0,1)):
        obj=tmp_path/(file+str(cache)+str(page)+'.o')
        subprocess.run([str(compiler),'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Os',
          '-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f','-fstack-usage',
          '-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_STORAGE_IO_HOST_TEST=1',
          f'-DPET_P4_STORAGE_UNIFORM_CACHE_ENABLE={cache}',f'-DPET_P4_STORAGE_UNIFORM_CACHE_APPLY={cache}',
          f'-DPET_P4_STORAGE_PAGE_CACHE_ENABLE={page}',f'-DPET_P4_STORAGE_PAGE_CACHE_APPLY={page}',
          '-I',str(ROOT/'tests/storage_io_stubs'),'-I',str(ROOT/'tests/program_probe_stubs'),
          '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),'-c',str(ROOT/'main'/(file+'.c')),'-o',str(obj)],check=True)
        frames=obj.with_suffix('.su').read_text().splitlines();assert frames
        for row in frames:
            _,amount,kind=row.split('\t');assert kind=='static' and int(amount)<=bound,row
