"""Acceptance-only extra slot, never package-configurable or a public change."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_catalog_extra_slot_requires_exact_private_complete_parent(tmp_path):
    path=tmp_path/'profile.c';path.write_text('#include "pet_p4_catalog_profile.h"\n_Static_assert(PET_P4_MINIAPP_CATALOG_MAX==EXPECTED,"capacity");\n')
    complete=['PET_COMPONENT_PROGRAM_ENABLED=1','PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
      'PET_COMPONENT_BACKGROUND_ENABLED=1','PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1']
    for flags,expected in (([],32),(complete,32),(complete+['PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1'],33)):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',f'-DEXPECTED={expected}',*[f'-D{f}' for f in flags],
          '-I',str(ROOT/'main'),'-c',str(path),'-o',str(tmp_path/'profile.o')],check=True)
    invalid=subprocess.run(['cc','-std=c11','-DEXPECTED=33','-DPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1',
      '-I',str(ROOT/'main'),'-c',str(path),'-o',str(tmp_path/'bad.o')],capture_output=True,text=True)
    assert invalid.returncode and 'Private acceptance capacity requires' in invalid.stderr
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_program_parent_acceptance':
            assert config[section]['extends']=='env:esp32_p4_program_parent_candidate' and 'PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1' in flags
        else:assert 'PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1' not in flags
