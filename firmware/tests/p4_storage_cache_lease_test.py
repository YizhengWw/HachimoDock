"""Native cache ownership only; not physical throughput/acoustic acceptance."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_native_cache_work_uses_actual_broker_and_releases_after_revoke_without_adopting_diagnostics(tmp_path):
    _run_cache_work(tmp_path,False)

def test_native_cache_batch_retains_deadline_and_rotates_only_retired_exact_epochs(tmp_path):
    _run_cache_work(tmp_path,True)

def _run_cache_work(tmp_path,batch):
    binary=tmp_path/'cache-work'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1',
        '-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1','-DPET_P4_STORAGE_PAGE_CACHE_APPLY=1',
        '-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1','-DPET_P4_STORAGE_CACHE_SCOPE_ENABLED=1',
        '-DPET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED=1',
        f'-DPET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED={int(batch)}',
        '-I',str(ROOT/'tests/storage_io_stubs'),'-I',str(ROOT/'main'),
        str(ROOT/'tests/p4_storage_cache_work_test.c'),str(ROOT/'main/pet_p4_resources.c'),
        str(ROOT/'main/pet_p4_resources_device.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_boot_cache_is_per_physical_item_not_a_skipped_validation_or_private_save_lease():
    from p4_render_trace_test import function
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    validate=function(code,'validate_catalog_snapshot')
    assert validate.index('for (size_t i')<validate.index('pet_storage_cache_work_begin(')<validate.index('load_catalog_runtime_item(')
    assert 'snapshot->items[i].resource_state = 3' in validate and 'complete = false' in validate
    boot_activate=function(code,'activate_catalog_index_in_memory')
    assert boot_activate.index('pet_storage_cache_work_begin(')<boot_activate.index('load_catalog_runtime(')<boot_activate.index('if(!loaded)')<boot_activate.index('restore_component_runtime(')
    assert 'cleanup(pet_storage_cache_work_finish)' in boot_activate
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    for name in c.sections():
        flags=c[name].get('build_flags','')
        if name=='env:esp32_p4_boot_cache_parent':
            assert c[name]['extends']=='env:esp32_p4_boot_trace_control'
            assert 'PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED=1' in flags
        else:assert 'PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED=1' not in flags

def test_actual_cache_lease_rejects_alias_stale_copy_wrong_task_and_external_window(tmp_path):
    binary=tmp_path/'cache-lease'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1',
        '-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1','-DPET_P4_STORAGE_PAGE_CACHE_APPLY=1',
        '-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1','-DPET_P4_STORAGE_CACHE_SCOPE_ENABLED=1',
        '-DPET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED=1',
        '-I',str(ROOT/'tests/storage_io_stubs'),'-I',str(ROOT/'main'),
        str(ROOT/'tests/p4_storage_cache_lease_test.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_native_save_scope_starts_after_admission_and_ends_before_physical_lease_finish():
    from p4_render_trace_test import function
    code=(ROOT/'main/pet_p4_app_store.c').read_text()
    begin=function(code,'save_io_begin');end=function(code,'save_io_end')
    cache=function(code,'save_io_cache_begin')
    assert 'pet_storage_cache_lease_begin(' not in begin
    assert begin.index('if(borrowed)')<begin.index('return true;')<begin.index('pet_resources_device_acquire(')<begin.index('io->owned=true;')
    assert 'if(io->owned)' in cache and 'pet_storage_cache_lease_begin(' in cache
    load=function(code,'load_with_authority')
    assert load.index('saves[i].loading=true;')<load.index('xSemaphoreGive(lock);',load.index('saves[i].loading=true;'))<load.index('save_io_cache_begin(')<load.index('read_slot(')
    assert end.index('pet_storage_cache_lease_finish(')<end.index('pet_resources_device_finish(')<end.index('pet_resources_device_revoke(')
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    for name in c.sections():
        flags=c[name].get('build_flags','')
        if name=='env:esp32_p4_storage_save_parent':
            assert c[name]['extends']=='env:esp32_p4_recorder_cost_probe'
            assert 'PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED=1' in flags
        else:assert 'PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED=1' not in flags
