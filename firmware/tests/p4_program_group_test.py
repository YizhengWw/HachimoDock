"""Unified native owner lifecycle, actual pure modules and synthetic admission."""
import json
import os
from pathlib import Path
import pytest
from p4_program_host_test import ROOT, FLAGS, run

CASES=['shared_sequence_projection','backpressure_single_pump','manual_stop_report',
       'audio_revoke','revoke_at_timer_enqueue','generation_aliases','callback_reentry',
       'replacement_isolation','clock_fault','sequence_exhaustion_optional','attach_ownership','bypassed_sequence',
       'unique_attachment_failed_restart','copied_group_rejected']
SOURCES=[ROOT/'main'/('pet_p4_'+name+'.c') for name in
         ('program_core','program_image','program_runtime','program_host','program_timers','event_bus','program_group')]

@pytest.fixture(scope='module')
def executable(tmp_path_factory):
    binary=tmp_path_factory.mktemp('program-group')/'group'
    run([os.environ.get('CC','cc'),*FLAGS,'-I',ROOT/'main','-I',ROOT/'tests',*SOURCES,
         ROOT/'tests/p4_program_group_test.c','-o',binary])
    return binary

@pytest.mark.parametrize('case',range(len(CASES)),ids=CASES)
def test_group_actual_native_lifecycle(executable,case):
    result=json.loads(run([executable,case]))
    assert result['ok'] and result['groupBytes']<=160

def test_group_is_not_installed_and_opens_no_effect_or_quota():
    for file in ('pet_p4_main.c','pet_p4_protocol.c','pet_p4_miniapp.c'):
        assert 'pet_program_group_' not in (ROOT/'main'/file).read_text()
    source=(ROOT/'main/pet_p4_program_group.c').read_text()
    for forbidden in ('malloc(', 'fopen(', 'vTaskDelay(', 'pet_resources_try_acquire(',
                      'gpio_', 'dma_', 'pet_program_persistence_', 'while ('):
        assert forbidden not in source
    assert '[PET_RESOURCE_PROGRAM]={0,0}' in (ROOT/'main/pet_p4_resources.c').read_text()

def test_group_p4_object_has_bounded_own_frames_and_native_dependency_surface(tmp_path):
    from p4_program_test import p4_compiler,WARNINGS,FREESTANDING,INCLUDES
    compiler=p4_compiler()
    if not compiler:pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj=tmp_path/'group.o'
    run([compiler,*WARNINGS,'-Os','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f',
         *FREESTANDING,'-fstack-usage',*INCLUDES,'-c',ROOT/'main/pet_p4_program_group.c','-o',obj])
    imports={line.split()[-1] for line in run([str(compiler).removesuffix('gcc')+'nm','-u',obj]).splitlines()}
    assert imports<={'memcpy','memset','pet_program_host_check_current','pet_program_host_enqueue',
        'pet_program_host_pump','pet_program_host_cancel','pet_program_host_snapshot','pet_program_host_take_report',
        'pet_program_host_project_current','pet_program_timers_tick','pet_program_timers_cancel',
        'pet_event_bus_deliver','pet_event_bus_close'}
    for row in obj.with_suffix('.su').read_text().splitlines():
        _,amount,kind=row.split('\t');assert kind=='static' and int(amount)<=256
