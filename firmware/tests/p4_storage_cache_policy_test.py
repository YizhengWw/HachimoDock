"""Mount-memory comparison boundaries; no format/migration, device or sound."""
from pathlib import Path
import configparser
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_mount_policy_stays_eight_and_cannot_silently_expand(tmp_path):
    source=tmp_path/'policy.c'
    source.write_text('#include "pet_p4_storage_policy.h"\n_Static_assert(PET_P4_SPIFFS_MAX_FILES==EXPECTED,"wrong pool");\nint main(void){return 0;}\n')
    for count,experiment,success in ((8,0,True),(8,1,True),(31,1,True),(31,0,False),(0,1,False),(32,1,False),(-1,1,False)):
        result=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
            f'-DPET_P4_SPIFFS_MAX_FILES={count}',f'-DPET_P4_STORAGE_CACHE_EXPERIMENT={experiment}',
            f'-DEXPECTED={count}',str(source),'-o',str(tmp_path/'policy')],capture_output=True,text=True)
        assert (result.returncode==0)==success,(count,experiment,result.stderr)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-DEXPECTED=8',
        str(source),'-o',str(tmp_path/'default')],check=True)
    code=(ROOT/'main/pet_p4_main.c').read_text().split('static esp_err_t init_storage(void) {',1)[1].split('static esp_err_t',1)[0]
    assert '.max_files = PET_P4_SPIFFS_MAX_FILES' in code
    assert '.format_if_mount_failed = false' in code
    diagnostic=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    assert '"spiffsMaxFiles", PET_P4_SPIFFS_MAX_FILES' in diagnostic

def test_only_independent_private_cache_target_expands_mount_memory():
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    private='env:esp32_p4_storage_cache_probe';control='env:esp32_p4_storage_cache_control'
    assert c['platformio']['default_envs']=='esp32_p4_evboard'
    assert c[private]['extends']==c[control]['extends']=='env:esp32_p4_scheduler_probe'
    assert '-DPET_P4_SPIFFS_MAX_FILES=31' in c[private]['build_flags']
    assert '-DPET_P4_STORAGE_CACHE_EXPERIMENT=1' in c[private]['build_flags']
    assert 'build_flags' not in c[control]
    for name in c.sections():
        if name==private:continue
        assert 'PET_P4_SPIFFS_MAX_FILES' not in c[name].get('build_flags','')
        assert private not in c[name].get('extends','')
