"""Owned generic list lifecycle, real native modules and synthetic authority."""
import json
import os
import pytest
from p4_program_host_test import ROOT,FLAGS,run
CASES=['owned_copy_scrub','explicit_seed','reorder_filter_context','transactional_replace',
       'gesture_consumption','missing_row_no_commit','stop_bank_reuse','authority_commit_loss',
       'read_refusal_history','callback_reentry','aliases_before_callbacks','bad_source_rollback',
       'binding_loss_no_foreign_release','10000_cached_paints','maximum_utf8_text','counter_exhaustion',
       '10000_shared_core_oracle_turns','render_capture_context_recheck','receiver_identity_before_first_bind',
       'foreign_bank_and_observation','initial_refusal_no_defaults']
@pytest.fixture(scope='module')
def executable(tmp_path_factory):
    binary=tmp_path_factory.mktemp('collection-host')/'host'
    run([os.environ.get('CC','cc'),*FLAGS,'-O1','-I',ROOT/'main',ROOT/'main/pet_p4_collection.c',
         ROOT/'main/pet_p4_collection_host.c',ROOT/'tests/p4_collection_host_test.c','-o',binary])
    return binary
@pytest.mark.parametrize('case',range(len(CASES)),ids=CASES)
def test_actual_owned_collection(executable,case):
    result=json.loads(run([executable,case]));assert result['ok']
    assert result['hostBytes']<=2304 and result['bankBytes']<=147456 and result['workspaceBytes']<=4096
def test_closed_component_boundary_and_cached_read_has_no_projection_or_io():
    from p4_render_trace_test import function
    for file in ('pet_p4_main.c','pet_p4_protocol.c','pet_p4_miniapp.c'):
        source=(ROOT/'main'/file).read_text()
        assert 'pet_collection_host_' not in source and 'pet_collection_dataset_prepare(' not in source
    source=(ROOT/'main/pet_p4_collection_host.c').read_text();read=function(source,'pet_collection_host_read')
    for forbidden in ('pet_collection_project','strlen(', 'for(', 'while('):assert forbidden not in read
    for forbidden in ('malloc(', 'fopen(', 'vTaskDelay(', 'gpio_', 'dma_', 'pet_resources_try_acquire('):assert forbidden not in source
    assert 'widgetCollection' not in (ROOT/'main/pet_p4_protocol.c').read_text()
def test_p4_own_frames_layout_and_dependencies(tmp_path):
    from p4_program_test import p4_compiler,WARNINGS,FREESTANDING,INCLUDES
    compiler=p4_compiler()
    if not compiler:pytest.skip('requires existing P4 compiler, never invokes PlatformIO')
    obj=tmp_path/'collection-host.o'
    run([compiler,*WARNINGS,'-Os','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f',
         *FREESTANDING,'-fstack-usage',*INCLUDES,'-c',ROOT/'main/pet_p4_collection_host.c','-o',obj])
    imports={line.split()[-1] for line in run([str(compiler).removesuffix('gcc')+'nm','-u',obj]).splitlines()}
    assert imports<={'memcpy','memset','memcmp','strlen','pet_collection_project','pet_collection_project_validated'}
    for row in obj.with_suffix('.su').read_text().splitlines():
        _,amount,kind=row.split('\t');assert kind=='static' and int(amount)<=256
    layout=tmp_path/'layout.c';layout.write_text('#include "pet_p4_collection_host.h"\n'
        '_Static_assert(sizeof(pet_collection_host_t)<=2304,"host");\n'
        '_Static_assert(sizeof(pet_collection_dataset_t)<=147456,"bank");\n'
        '_Static_assert(sizeof(pet_collection_host_workspace_t)<=4096,"scratch");\n')
    run([compiler,*WARNINGS,'-Os','-march=rv32imafc_zicsr_zifencei_xesppie','-mabi=ilp32f',*FREESTANDING,*INCLUDES,'-c',layout,'-o',tmp_path/'layout.o'])
