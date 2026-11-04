"""Private cursor hints: real installed SDK on simulated RAM flash, no device."""
from pathlib import Path
import configparser
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]
STUBS=ROOT/'tests/spiffs_hint_stubs'
SDK=Path.home()/'.platformio/packages/framework-espidf/components/spiffs'

@pytest.mark.parametrize('apply',(0,1))
def test_unmodified_sdk_lookup_cow_rename_delete_remount_and_io_errors(tmp_path,apply):
    assert (SDK/'spiffs/src/spiffs_nucleus.c').is_file(),'installed SDK source required'
    common=['cc','-std=c11','-g','-O1','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',
      '-I',str(STUBS),'-I',str(SDK/'include'),'-I',str(SDK/'spiffs/src'),'-I',str(ROOT/'main')]
    objects=[]
    for name in ('spiffs_cache','spiffs_check','spiffs_gc','spiffs_hydrogen','spiffs_nucleus'):
        obj=tmp_path/(name+'.o');extra=[]
        if name in ('spiffs_hydrogen','spiffs_nucleus'):
            symbol='__wrap_' if name=='spiffs_hydrogen' else '__real_'
            extra=['-Dspiffs_object_find_object_index_header_by_name='+symbol+'spiffs_object_find_object_index_header_by_name']
        # Vendor SPIFFS intentionally uses packed unaligned u16 indices with
        # SPIFFS_ALIGNED_OBJECT_INDEX_TABLES=0. Keep its actual format/config;
        # disable only that vendor alignment check, not ASan/other UB checks.
        # Our wrapper/harness below retains the full alignment sanitizer.
        subprocess.run([*common,'-fno-sanitize=alignment',*extra,'-c',str(SDK/'spiffs/src'/(name+'.c')),'-o',str(obj)],check=True)
        objects.append(str(obj))
    binary=tmp_path/'ram-flash'
    subprocess.run([*common,'-Wall','-Wextra','-Werror','-DPET_SPIFFS_HINT_HOST_TEST=1','-DPET_P4_SPIFFS_HINT_ENABLE=1',
      f'-DPET_P4_SPIFFS_HINT_APPLY={apply}',str(ROOT/'tests/p4_spiffs_hint_test.c'),*objects,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30)

def test_ordinary_image_has_no_wrapper_hint_ram_or_private_grant(tmp_path):
    obj=tmp_path/'off.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',
      str(ROOT/'main/pet_p4_spiffs_hint.c'),'-o',str(obj)],check=True)
    symbols=subprocess.run(['nm',str(obj)],capture_output=True,text=True,check=True).stdout
    for private in ('__wrap_','hints','hint_mux','stats'):assert private not in symbols
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    for section in c.sections():
        if section in ('env:esp32_p4_storage_lookup_probe','env:esp32_p4_storage_lookup_control'):continue
        if section=='env:esp32_p4_lookup_parent':
            assert c[section]['extends']=='env:esp32_p4_save_budget_parent'
            flags=c[section]['build_flags']
            for expected in ('PET_P4_SPIFFS_HINT_ENABLE=1','PET_P4_SPIFFS_HINT_APPLY=1',
                             '-Wl,--wrap=spiffs_object_find_object_index_header_by_name'):
                assert flags.count(expected)==1
            assert 'PET_P4_SPIFFS_MAX_FILES=' not in flags
            continue
        assert 'PET_P4_SPIFFS_HINT_' not in c[section].get('build_flags','')
        assert 'env:esp32_p4_storage_lookup_' not in c[section].get('extends','')
    source=(ROOT/'main/pet_p4_spiffs_hint.c').read_text()
    for forbidden in ('malloc(', 'fopen(', 'ESP_LOG', 'printf(', 'SPIFFS_format(', 'SPIFFS_remove('):assert forbidden not in source

def test_sdk_lookup_call_sites_are_under_real_fs_lock():
    from p4_render_trace_test import function
    hydrogen=(SDK/'spiffs/src/spiffs_hydrogen.c').read_text()
    nucleus=(SDK/'spiffs/src/spiffs_nucleus.c').read_text()
    finder='spiffs_object_find_object_index_header_by_name'
    for caller in ('SPIFFS_open','SPIFFS_remove','SPIFFS_stat','SPIFFS_rename','SPIFFS_update_meta'):
        body=function(hydrogen,caller)
        assert body.index('SPIFFS_LOCK(fs)')<body.index(finder)<body.rindex('SPIFFS_UNLOCK(fs)')
    visitor=function(nucleus,finder+'_v')
    assert '_spiffs_rd(' in visitor and 'strcmp(' in visitor and 'SPIFFS_PH_FLAG_DELET' in visitor
    assert 'spiffs_obj_lu_find_entry_visitor' in function(nucleus,finder)
