"""Exact SPIFFS v3 guard and actual production SDK object/link checks; never PIO."""
import json
import configparser
import os
import re
import shlex
import subprocess
from pathlib import Path
import pytest
ROOT = Path(__file__).resolve().parents[1]
NAMES = ('pet_p4_storage_io_trace.c','pet_p4_spiffs_hint.c')


@pytest.mark.parametrize('name', NAMES)
def test_spiffs_guard_exact_sdk_revision_and_tee(tmp_path, name):
    text = (ROOT/'main'/name).read_text()
    start = text.index('#if !defined(CONFIG_IDF_TARGET_ESP32P4)')
    guard = text[start:text.index('#endif',start)+6]
    for version,lo,hi,tee,target,good in [(50501,1,199,0,1,True),(50504,300,399,0,1,True),
        (50504,1,199,0,1,False),(50504,300,400,0,1,False),(50504,300,399,1,1,False),
        (50504,300,399,0,0,False),(50505,300,399,0,1,False)]:
        p = tmp_path/'guard.c'
        p.write_text(f'#define ESP_IDF_VERSION {version}\n#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define CONFIG_ESP_REV_MIN_FULL {lo}\n#define CONFIG_ESP_REV_MAX_FULL {hi}\n#define CONFIG_SECURE_ENABLE_TEE {tee}\n'+('#define CONFIG_IDF_TARGET_ESP32P4 1\n' if target else '')+guard)
        result = subprocess.run(['cc','-E',str(p)],capture_output=True,text=True)
        assert (result.returncode==0)==good, result.stderr


def test_actual_v3_spiffs_sdk_and_production_hal_lookup_link(tmp_path):
    path = Path(os.environ.get('PET_SPIFFS_COMPILE_COMMANDS',ROOT/'.pio/build/esp32_p4_program_public_candidate_v3/compile_commands.json'))
    if not path.is_file():pytest.skip('requires existing source-bound v3 compile_commands; never invokes PIO')
    commands = json.loads(path.read_text())
    targets = list(NAMES)+['spiffs_api.c','spiffs_cache.c','spiffs_check.c','spiffs_gc.c','spiffs_hydrogen.c','spiffs_nucleus.c','esp_spiffs.c']
    entries = [next(x for x in commands if Path(x['file']).name==name) for name in targets]
    sdk = Path(entries[-2]['file']).parents[4]
    assert 'set(IDF_VERSION_PATCH 4)' in (sdk/'tools/cmake/version.cmake').read_text()
    # PIO adds profile CPP flags after CMake exports compile_commands. Resolve
    # the exact inherited profile, preserving public -U ordering, rather than
    # accidentally testing default-off empty wrapper objects.
    cfg = configparser.ConfigParser(interpolation=None);cfg.read(ROOT/'platformio.ini')
    def option(section,key):
        if not cfg.has_option(section,key):return option(cfg.get(section,'extends'),key)
        value=cfg.get(section,key)
        return re.sub(r'\$\{([^}]+)\}',lambda m:option(*m[1].rsplit('.',1)),value)
    cpp=[arg for arg in shlex.split(option('env:esp32_p4_program_public_candidate_v3','build_flags')) if arg.startswith(('-D','-U'))]
    assert '-DPET_P4_STORAGE_IO_TRACE_ENABLE=1' in cpp and '-DPET_P4_SPIFFS_HINT_ENABLE=1' in cpp
    assert '-DPET_P4_PPA_QUEUE_PROBE=1' not in cpp
    objects = []
    cc = None
    for entry in entries:
        args = entry.get('arguments') or shlex.split(entry['command'])
        cc = args[0];clean = [cc];index = 1
        # Retain exact production macros/config/include/architecture. Redirect
        # ALL outputs away from the preserved failed build directory.
        while index<len(args):
            arg=args[index]
            if arg in ('-o','-MF','-MT','-MQ'):index+=2;continue
            if arg in ('-MD','-MMD','-MP'):index+=1;continue
            clean.append(arg);index+=1
        obj = tmp_path/(Path(entry['file']).name+'.o')
        result = subprocess.run([*clean,*cpp,'-o',str(obj)],cwd=entry['directory'],capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        objects.append(obj)
    wraps = ['spiffs_api_read','spiffs_api_write','spiffs_api_erase','spiffs_object_find_object_index_header_by_name']
    linked = tmp_path/'spiffs-linked.o'
    result = subprocess.run([cc,'-nostdlib','-r',*[str(p) for p in objects],*['-Wl,--wrap='+n for n in wraps],'-o',str(linked)],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    nm = subprocess.run([cc.replace('gcc','nm'),str(linked)],check=True,capture_output=True,text=True).stdout
    for name in wraps:
        assert re.search(r'\bT\s+__wrap_'+name+r'$',nm,re.M),name
        assert re.search(r'\bT\s+'+name+r'$',nm,re.M),name
        assert not re.search(r'\bU\s+__real_'+name+r'$',nm,re.M),name
    objdump = cc.replace('gcc','objdump')
    reloc = subprocess.run([objdump,'-r',str(linked)],check=True,capture_output=True,text=True).stdout
    # Real SDK esp_spiffs.c HAL callback registrations must link to wrappers,
    # not just contain matching wrapper function symbols elsewhere in ELF.
    for name in wraps[:3]:
        for kind in ('R_RISCV_HI20','R_RISCV_LO12_I'):
            assert re.search(kind+r'\s+__wrap_'+name+r'\b',reloc),(name,kind)
    # Real SDK hydrogen callers must link to the lookup wrapper.
    assert re.search(r'R_RISCV_CALL(?:_PLT)?\s+__wrap_'+wraps[3]+r'\b',reloc)
    # Every wrapper's __real_ reference resolves to its ACTUAL SDK provider.
    for name in wraps:
        body=subprocess.run([objdump,'-dr','--disassemble=__wrap_'+name,str(linked)],check=True,capture_output=True,text=True).stdout
        assert re.search(r'R_RISCV_CALL(?:_PLT)?\s+'+name+r'\b',body),name
    # Audit real lookup call sites remain under the SDK fs lock.
    from p4_render_trace_test import function
    hydrogen=(sdk/'components/spiffs/spiffs/src/spiffs_hydrogen.c').read_text()
    for caller in ('SPIFFS_open','SPIFFS_remove','SPIFFS_stat','SPIFFS_rename','SPIFFS_update_meta'):
        body=function(hydrogen,caller)
        assert body.index('SPIFFS_LOCK(fs)')<body.index(wraps[3])<body.rindex('SPIFFS_UNLOCK(fs)')
    vfs=(sdk/'components/spiffs/esp_spiffs.c').read_text()
    for callback in wraps[:3]:assert re.search(r'hal_\w+_f\s*=\s*'+callback+r'\s*;',vfs)


@pytest.mark.parametrize('apply', (0,1))
def test_v3_real_sdk_lookup_runtime_preserves_io_errors_and_media(tmp_path, apply):
    path=Path(os.environ.get('PET_SPIFFS_COMPILE_COMMANDS',ROOT/'.pio/build/esp32_p4_program_public_candidate_v3/compile_commands.json'))
    if not path.is_file():pytest.skip('requires existing pinned v3 SDK build metadata')
    entry=next(x for x in json.loads(path.read_text()) if Path(x['file']).name=='spiffs_nucleus.c')
    sdk=Path(entry['file']).parents[2]
    import p4_spiffs_hint_test as original
    prior=original.SDK
    try:
        original.SDK=sdk
        original.test_unmodified_sdk_lookup_cow_rename_delete_remount_and_io_errors(tmp_path,apply)
        original.test_sdk_lookup_call_sites_are_under_real_fs_lock()
    finally:original.SDK=prior
