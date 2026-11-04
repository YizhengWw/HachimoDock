"""Actual foreground/registry parent functions, real broker/VM, no device IO.

Only the clock, nonrecursive mutex, preloaded save adapter and UI projection
sink are synthetic. The production quota is compiled, never replaced by a
test backend. Normal and old private profiles retain their existing limits.
"""
from pathlib import Path
import configparser
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def _actual_parent_case(tmp_path,prefetch):
    from p4_render_trace_test import function
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    background=(ROOT/'main/pet_p4_miniapp_background.inc').read_text()
    harness=(ROOT/'tests/p4_program_registry_test.c').read_text().split('static void turn(void)')[0]
    code=harness+r'''
#define EXT_RAM_BSS_ATTR
#define MINIAPP_VAR_INT 1
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
typedef struct {
 bool active,program_needs_start,program_seed_ready,program_suspended,program_background_owned;
 unsigned var_count;struct {int type;int32_t int_value;} vars[8];
 char widget_id[48];uint64_t program_content[3];
 pet_component_program_config_t program;pet_program_snapshot_t program_seed;
} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static struct {bool occupied;} g_storage_lane;
static pet_component_program_session_t g_component_program,third;
static uint32_t g_image_revision=1,g_viewport_epoch,g_component_program_attempt_revision;
static bool g_component_program_initialized,g_component_program_scheduler_initialized;
static uint32_t g_component_program_last_us,g_component_program_max_us;
static pet_program_runtime_result_t g_component_program_start_result;
static pet_program_registry_t g_background_programs;
static void *g_background_mutex=(void*)1;
static bool g_background_ready;
static uint32_t g_background_last_us,g_background_max_us;
#if PET_P4_H264_PREFETCH
static bool g_program_slice_admitted=true;
#endif
static unsigned depth,lock_calls,unlocks,projections;
static int xSemaphoreTake(void *s,unsigned timeout){assert(s==(void*)1&&timeout==portMAX_DELAY&&!depth);depth++;lock_calls++;return pdTRUE;}
static void xSemaphoreGive(void *s){assert(s==(void*)1&&depth==1);depth--;unlocks++;}
static void refresh_view(miniapp_runtime_t *r){assert(r==&g_runtime&&depth==1);projections++;}
'''
    names=('component_parent_enter','component_parent_leave','component_program_retire_locked',
           'component_program_snapshot_locked','component_program_values','component_program_process_locked',
           'component_program_visibility_locked')
    for name in names:
        if name=='component_program_process_locked':
            code+=function(background,'background_matches_under_registry')+'\n'
            code+=function(background,'background_project_locked')+'\n'
        code+=function(text,name)+'\n'
    code+=function(background,'background_process_locked')+r'''
static void cycle(uint64_t now,bool visible){
 component_program_visibility_locked(visible);
 background_process_locked(now);
 component_program_process_locked(now,visible);
 assert(!depth&&lock_calls==unlocks);
}
int main(void){
 assert(pet_resources_limit(PET_RESOURCE_PROGRAM).leases==2&&pet_resources_limit(PET_RESOURCE_PROGRAM).units==4096);
 assert(pet_program_registry_init(&g_background_programs,pet_program_host_device_init));g_background_ready=true;
 config.enabled=true;config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};
 const pet_program_insn_t instructions[]={
 {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
 {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
 memcpy(config.image.code,instructions,sizeof(instructions));
 config.projection=(pet_program_ui_projection_t){.schema=1,.variables=2,.minimum={0,0},.maximum={100,100}};
 memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(config.projection.target));config.projection.target[0]=0;
 seed.schema=1;seed.cells=1;const uint64_t content[3]={11,22,33};
 pet_program_registration_t bg={0};
 assert(pet_program_registry_attach(&g_background_programs,"alpha",content,&config,&seed,base,true,0,&bg)==PET_REGISTRY_OK);
 g_runtime=(miniapp_runtime_t){.active=true,.program_needs_start=true,.program_seed_ready=true,
   .var_count=2,.vars={{MINIAPP_VAR_INT,0},{MINIAPP_VAR_INT,7}},.widget_id="beta",.program=config,.program_seed=seed};
#if PET_P4_H264_PREFETCH
 g_program_slice_admitted=false;component_program_process_locked(0,true);
 assert(g_runtime.program_needs_start&&!g_component_program_initialized&&!g_runtime.program_suspended);
 g_program_slice_admitted=true;
#endif
 component_program_process_locked(0,true);
 assert(g_component_program_start_result==PET_RUNTIME_OK&&g_component_program.live&&g_component_program.dispatch==&g_background_programs.scheduler);
 assert(!g_component_program.scheduler.instance&&g_component_program.scheduled);
 pet_resource_usage_t usage;pet_resources_device_usage(&usage);
 assert(usage.leases[PET_RESOURCE_PROGRAM]==2&&usage.units[PET_RESOURCE_PROGRAM]==128);
 assert(pet_program_host_device_init(&third.host));
 assert(pet_component_program_session_start(&third,&config,&seed,1,0)==PET_RUNTIME_BUSY&&!third.host.bound);
 pet_program_event_t fg_event={1,{10}},bg_event={1,{1}};
 assert(pet_component_program_session_enqueue(&g_component_program,1,&fg_event)==PET_RUNTIME_OK);
 assert(pet_component_program_session_enqueue(&g_background_programs.entries[bg.slot].session,bg.serial,&bg_event)==PET_RUNTIME_OK);
#if PET_P4_H264_PREFETCH
 g_program_slice_admitted=false;cycle(0,true);
 assert(g_component_program.host.runtime.queued==1&&g_background_programs.entries[bg.slot].session.host.runtime.queued==1);
 assert(!g_component_program.host.runtime.inflight&&!g_background_programs.entries[bg.slot].session.host.runtime.inflight);
 g_program_slice_admitted=true;
#endif
 /* Foreground projection is NOT a second scheduler pump. */
 for(unsigned i=0;i<10;i++)component_program_process_locked(0,true);
 assert(!g_component_program.host.runtime.inflight&&g_component_program.host.runtime.queued==1);
 assert(g_background_programs.entries[bg.slot].session.host.runtime.queued==1&&!projections);
 for(unsigned t=1;t<16;t++)cycle(t,true);
 assert(g_runtime.vars[0].int_value==10&&g_runtime.vars[1].int_value==7&&projections==1);
 assert(g_background_programs.entries[bg.slot].committed.values[0]==1);
 pet_program_snapshot_t retained;assert(component_program_snapshot_locked(&retained)&&retained.values[0]==10);
 uint64_t prior_generation=g_component_program.generation;
 assert(pet_component_program_session_enqueue(&g_component_program,1,&fg_event)==PET_RUNTIME_OK);
#if PET_P4_H264_PREFETCH
 g_program_slice_admitted=false;
#endif
 cycle(20,false); /* No queued foreground execution after hide, even one turn. */
 assert(g_runtime.program_suspended&&!g_component_program.live&&!g_component_program.scheduled);
 assert(component_program_snapshot_locked(&retained)&&retained.values[0]==10);
 assert(g_background_programs.entries[bg.slot].session.live);
#if PET_P4_H264_PREFETCH
 g_program_slice_admitted=true;
#endif
 /* Count and fuel are independent real aggregate limits, not per-child 4096. */
 static pet_component_program_session_t overfuel,heavy;
 pet_component_program_config_t cost=config;cost.image.fuel=4060;
 assert(pet_program_host_device_init(&overfuel.host));
 assert(pet_component_program_session_start(&overfuel,&cost,&seed,1,20)==PET_RUNTIME_BUSY&&!overfuel.host.bound);
 cost.image.fuel=4000;assert(pet_program_host_device_init(&heavy.host));
 assert(pet_component_program_session_start(&heavy,&cost,&seed,1,20)==PET_RUNTIME_OK);
 pet_resources_device_usage(&usage);assert(usage.leases[PET_RESOURCE_PROGRAM]==2&&usage.units[PET_RESOURCE_PROGRAM]==4064);
 assert(pet_component_program_session_stop(&heavy)==PET_RUNTIME_OK);
 for(unsigned t=21;t<30;t++)cycle(t,false);
 assert(!g_component_program.live&&retained.values[0]==10);
 /* The navigation-publication gap does not latch a stop for a new lifecycle. */
 g_image_revision++;g_runtime.program_suspended=false;g_runtime.program_needs_start=true;g_storage_lane.occupied=true;
 cycle(30,false);assert(g_runtime.program_needs_start&&!g_runtime.program_suspended&&!g_component_program.live);
 g_storage_lane.occupied=false;cycle(31,true);
 assert(g_component_program.live&&g_component_program.generation>prior_generation);
 assert(component_program_snapshot_locked(&retained)&&retained.values[0]==10);
 /* A real reservation revokes BOTH children before sampled audio flags. */
 pet_resource_realtime_t audio={0};assert(pet_resources_device_realtime_begin(&audio)==PET_RESOURCE_OK);
 cycle(32,true);cycle(33,true);
 assert(!g_component_program.live&&!g_background_programs.entries[bg.slot].session.live);
 pet_resources_device_usage(&usage);assert(!usage.leases[PET_RESOURCE_PROGRAM]);
 pet_resources_device_realtime_end(&audio);cycle(34,true);
 assert(!g_component_program.live&&!g_background_programs.entries[bg.slot].session.live);
 assert(g_background_programs.entries[bg.slot].committed.values[0]==1);
 assert(pet_program_registry_checkpoint_one(&g_background_programs)&&saved.values[0]==1&&checkpoints==1);
 assert(pet_program_registry_release(&g_background_programs,bg)&&!pinned[0]);
 assert(lock_calls==unlocks&&!depth);
 return 0;
}
'''
    # The harness declarations retained above are intentionally shared with
    # the established exact-owner tests; unrelated helpers are not linked.
    code=code.replace('static pet_program_registry_t registry;','')
    code=code.replace('static uint64_t now_ms;','')
    code=code.replace('static int32_t base[8]={0,7},values[8];','static int32_t base[8]={0,7};')
    path=tmp_path/'parent.c';path.write_text(code);binary=tmp_path/'parent'
    sources=('resources','resources_device','component_program','component_program_session','program_registry',
      'program_core','program_image','program_runtime','program_host','program_host_device','program_group',
      'program_scheduler','program_timers','event_bus')
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
      '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1',f'-DPET_P4_H264_PREFETCH={prefetch}',
      '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),'-I',str(vendor),str(path),
      *[str(ROOT/'main'/f'pet_p4_{n}.c') for n in sources],str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)

def test_actual_unified_parent_one_pump_hidden_stop_and_real_aggregate_admission(tmp_path):
    _actual_parent_case(tmp_path,0)

def test_physical_native_work_defers_new_program_work_but_never_hiding_or_accepted_events(tmp_path):
    _actual_parent_case(tmp_path,1)

def test_unified_parent_is_closed_without_complete_backend_and_normal_limits_unchanged(tmp_path):
    path=tmp_path/'gates.c';path.write_text('''
#include <assert.h>
#include "pet_p4_component_features.h"
#include "pet_p4_resources.h"
int main(void){assert(pet_resources_limit(PET_RESOURCE_PROGRAM).leases==EXPECTED);return 0;}
''')
    for defines,expected in (([],0),(['PET_COMPONENT_PROGRAM_ENABLED=1'],0),
        (['PET_COMPONENT_PROGRAM_ENABLED=1','PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1'],1),
        (['PET_COMPONENT_PROGRAM_ENABLED=1','PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','PET_COMPONENT_BACKGROUND_ENABLED=1'],1),
        (['PET_COMPONENT_PROGRAM_ENABLED=1','PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','PET_COMPONENT_BACKGROUND_ENABLED=1','PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1'],2)):
        binary=tmp_path/'gates'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',f'-DEXPECTED={expected}',*[f'-D{d}' for d in defines],
          '-I',str(ROOT/'main'),str(path),str(ROOT/'main/pet_p4_resources.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    rejected=subprocess.run(['cc','-std=c11','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1','-I',str(ROOT/'main'),'-c',str(path),'-o',str(tmp_path/'bad.o')],capture_output=True,text=True)
    assert rejected.returncode and 'Unified program parent requires' in rejected.stderr
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_program_parent_candidate':
            assert config[section]['extends']=='env:esp32_p4_background_candidate' and 'PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1' in flags
        elif section=='env:esp32_p4_program_public_candidate':
            assert config[section]['extends']=='env:esp32_p4_capability_parent' and 'PET_P4_PUBLIC_PROGRAM_PROFILE=1' in flags
        else:assert 'PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1' not in flags
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    from p4_render_trace_test import function
    main=function(text,'pet_p4_miniapp_process')
    assert main.index('component_program_visibility_locked(visible)')<main.index('background_process_locked(now_ms)')<main.index('component_program_process_locked(now_ms,visible)')
