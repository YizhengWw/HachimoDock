"""R142 framework parent env: rides the verified r143 PPA-copy chain with the parent's exact build flags (no new switch),
its own version label, and the same sdkconfig; the public candidate rides it. Offline only."""
from pathlib import Path
import configparser

ROOT=Path(__file__).resolve().parents[1]


def test_framework_parent_adds_no_switch_and_carries_the_public_candidate():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    env=config['env:esp32_p4_framework_parent']
    assert env['extends']=='env:esp32_p4_ppa_copy_parent' and env['custom_p4_project_version']=='0.7.103-p4-r142-framework'
    lines=[l.strip() for l in env['build_flags'].split('\n') if l.strip()]
    assert lines==['${env:esp32_p4_ppa_copy_parent.build_flags}']
    assert not any(v in env['build_flags'] for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
    # r145: the main-frame parent rides the framework parent and carries the public candidate.
    assert config['env:esp32_p4_main_frame_parent']['extends']=='env:esp32_p4_framework_parent'
    # r150: the shared capability parent rides the main-frame parent and carries the public candidate.
    assert config['env:esp32_p4_capability_parent']['extends']=='env:esp32_p4_main_frame_parent'
    assert config['env:esp32_p4_program_public_candidate']['extends']=='env:esp32_p4_capability_parent'
    for name in config.sections():
        if name not in ('env:esp32_p4_framework_parent','env:esp32_p4_main_frame_parent'):
            assert 'esp32_p4_framework_parent' not in config[name].get('extends','')+config[name].get('build_flags','')
    assert config['platformio']['default_envs']=='esp32_p4_evboard'
    assert (ROOT/'sdkconfig.esp32_p4_framework_parent').read_bytes()==(ROOT/'sdkconfig.esp32_p4_large_reply_parent').read_bytes()
