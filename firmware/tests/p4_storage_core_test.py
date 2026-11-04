"""R132 private storage-worker core isolation: pinned creation only, nothing else changes."""
from pathlib import Path
import configparser
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def test_storage_workers_core1_is_flag_gated_and_keeps_priorities_and_lanes(tmp_path):
    main=(ROOT/'main/pet_p4_main.c').read_text();store=(ROOT/'main/pet_p4_app_store.c').read_text();policy=(ROOT/'main/pet_p4_task_policy.h').read_text()
    assert '#define PET_P4_STORAGE_WORKER_CORE 1' in policy and '#define PET_P4_STORAGE_WORKERS_CORE1 0' in policy
    assert '#define PET_P4_STORAGE_IO_TASK_PRIORITY 1U' in policy and '#define PET_P4_PROTOCOL_TASK_PRIORITY 11U' in policy
    assert main.count('pet_p4_storage_worker_create(\n      protocol_worker_task,\n      "pet_p4_protocol",\n      PET_P4_PROTOCOL_TASK_STACK_BYTES,\n      NULL,\n      PET_P4_PROTOCOL_TASK_PRIORITY,\n      &g_protocol_task\n    )')==1
    assert 'xTaskCreatePinnedToCore(fn,name,stack,arg,priority,handle,PET_P4_STORAGE_WORKER_CORE)' in policy and 'xTaskCreate(fn,name,stack,arg,priority,handle)' in policy
    assert store.count('pet_p4_storage_worker_create(save_worker,"pet_app_save",PET_APP_SAVE_TASK_STACK_BYTES,NULL,1,&worker)')==1
    assert 'xTaskCreatePinnedToCore' not in store and 'xTaskCreatePinnedToCore' not in main.split('protocol_worker_task,')[0][-400:]
    # The unlocked-storage priority drop/restore is untouched: no new vTaskPrioritySet or affinity calls.
    assert main.count('vTaskPrioritySet(')==2 and 'vTaskCoreAffinitySet' not in main and 'vTaskCoreAffinitySet' not in store
    assert 'PET_P4_STORAGE_WORKERS_CORE1' not in (ROOT/'main/pet_p4_renderer.c').read_text()
    for name in ('pet_p4_audio.c','pet_p4_h264.c'):
        p=ROOT/'main'/name
        if p.exists():assert 'PET_P4_STORAGE_WORKERS_CORE1' not in p.read_text()
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_storage_core_parent':
            assert config[section]['extends']=='env:esp32_p4_commit_lean_parent'
            assert config[section]['custom_p4_project_version']=='0.7.103-p4-r132-core' and 'PET_P4_STORAGE_WORKERS_CORE1=1' in flags
            assert not any(v in flags for v in ('TASK_PRIORITY','PROGRAM_EXECUTION','F_CPU','PERIPHERAL','BUDGET_US','WATCHDOG','SCHEDULING_REFERENCE'))
        else:assert 'PET_P4_STORAGE_WORKERS_CORE1=1' not in flags
    # Compile check of the policy header under both settings.
    for define in ((),('-DPET_P4_STORAGE_WORKERS_CORE1=1',)):
        src=tmp_path/'policy.c';src.write_text('#include "pet_p4_task_policy.h"\nint main(void){return PET_P4_STORAGE_WORKER_CORE==1?0:1;}\n')
        assert subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*define,'-I',str(ROOT/'main'),'-fsyntax-only',str(src)],capture_output=True,text=True).returncode==0
