"""Default-closed actual native cooperative dispatcher, synthetic admission only."""
import json
import os
import pytest
from p4_program_host_test import ROOT,FLAGS,run
CASES=['round_robin_single_quantum','pending_report_no_starvation','timer_bus_fairness',
       'capacity_unique_binding','exact_slot_reuse','new_host_isolation','busy_stop_latch',
       'clock_fault','audio_stop','aliases_stale','callback_reentry','serial_exhaustion','10000_turns',
       'registration_reentry','busy_group_fairness','uncertain_retirement_keeps_slot','remove_busy_no_resume','copied_scheduler_rejected',
       'explicit_rearm_no_ticket_reuse']
SOURCES=[ROOT/'main'/('pet_p4_'+name+'.c') for name in
         ('program_core','program_image','program_runtime','program_host','program_timers','event_bus','program_group','program_scheduler')]
@pytest.fixture(scope='module')
def executable(tmp_path_factory):
    binary=tmp_path_factory.mktemp('program-scheduler')/'scheduler'
    run([os.environ.get('CC','cc'),*FLAGS,'-I',ROOT/'main',*SOURCES,
         ROOT/'tests/p4_program_scheduler_test.c','-o',binary])
    return binary
@pytest.mark.parametrize('case',range(len(CASES)),ids=CASES)
def test_actual_scheduler(executable,case):
    result=json.loads(run([executable,case]));assert result['ok'] and result['schedulerBytes']<=256
def test_scheduler_has_no_installed_route_or_effect():
    for file in ('pet_p4_main.c','pet_p4_protocol.c'):
        assert 'pet_program_scheduler_' not in (ROOT/'main'/file).read_text()
    # The serialized component parent is now an intentional native caller;
    # source-name absence is no longer the safety boundary for that parent.
    parent=(ROOT/'main/pet_p4_miniapp.c').read_text()
    process=parent.split('static void component_program_process_locked(',1)[1].split('#endif\n/* Per-call PSRAM workspace:',1)[0]
    assert parent.count('pet_program_scheduler_step(')==1
    assert process.index('if(!visible)')<process.index('pet_program_scheduler_step(')
    assert 'pet_component_program_session_bind_scheduler(&g_component_program,&g_component_program_scheduler)' in process
    assert 'pet_component_program_session_collect(&g_component_program' in process
    assert 'pet_component_program_session_step(&g_component_program' not in process
    assert 'pet_program_scheduler_' not in parent.split('static bool dispatch_action_observed(',1)[1].split('void pet_p4_miniapp_process(',1)[0]
    source=(ROOT/'main/pet_p4_program_scheduler.c').read_text()
    for forbidden in ('malloc(', 'fopen(', 'vTaskDelay(', 'pet_resources_try_acquire(',
                      'gpio_', 'dma_', 'pet_program_persistence_', 'while ('):
        assert forbidden not in source
    assert '[PET_RESOURCE_PROGRAM]={0,0}' in (ROOT/'main/pet_p4_resources.c').read_text()
    session=(ROOT/'main/pet_p4_component_program_session.c').read_text()
    assert 'pet_program_scheduler_step(&s->scheduler,now,&diagnostic)' in session
    assert 'pet_program_group_enqueue(&s->group' in session
    assert 'pet_program_host_pump(' not in session and 'pet_program_timers_tick(' not in session
def test_p4_own_frames_and_dependencies(tmp_path):
    from p4_program_test import p4_compiler,WARNINGS,FREESTANDING,INCLUDES
    compiler=p4_compiler()
    if not compiler:pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj=tmp_path/'scheduler.o'
    run([compiler,*WARNINGS,'-Os','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f',
         *FREESTANDING,'-fstack-usage',*INCLUDES,'-c',ROOT/'main/pet_p4_program_scheduler.c','-o',obj])
    imports={line.split()[-1] for line in run([str(compiler).removesuffix('gcc')+'nm','-u',obj]).splitlines()}
    assert imports<={'memcpy','memset','pet_program_group_check_current','pet_program_group_cancel',
                     'pet_program_group_tick','pet_program_group_deliver','pet_program_group_pump'}
    for row in obj.with_suffix('.su').read_text().splitlines():
        _,amount,kind=row.split('\t');assert kind=='static' and int(amount)<=256
