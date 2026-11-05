"""Real descriptor/session/interpreter; no device, real audio, files or cloud."""
from pathlib import Path
import copy
import importlib.util
import json
import subprocess
import sys
import pytest
ROOT=Path(__file__).resolve().parents[1]
SKILL=ROOT.parent/'pc/skills/petui'
sys.path.insert(0,str(SKILL/'scripts'))
SUITE=json.loads((SKILL/'contracts/component-program-v1.json').read_text())
def patched(value,patch):
    value=copy.deepcopy(value);o=value;parts=patch['path'].strip('/').split('/')
    for k in parts[:-1]:o=o[int(k)] if isinstance(o,list) else o[k]
    o[int(parts[-1]) if isinstance(o,list) else parts[-1]]=patch['value'];return value
@pytest.fixture(scope='module')
def native(tmp_path_factory):
    directory=tmp_path_factory.mktemp('component-program');vendor=ROOT/'tests/vendor/cjson';bins={}
    for enabled,resume in ((0,False),(1,False),(1,True)):
        binary=directory/f'program-{enabled}-{int(resume)}'
        flags=['-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_COMPONENT_BACKGROUND_ENABLED=1',
            '-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1','-DPET_COMPONENT_PROGRAM_RAM_RESUME=1'] if resume else []
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',
            '-fsanitize=address,undefined',f'-DPET_COMPONENT_PROGRAM_ENABLED={enabled}',*flags,'-I',str(ROOT/'main'),'-I',str(vendor),
            str(ROOT/'tests/p4_component_program_test.c'),str(ROOT/'main/pet_p4_component_program.c'),str(ROOT/'main/pet_p4_component_program_session.c'),
            *[str(ROOT/'main'/f'pet_p4_program_{name}.c') for name in ('core','image','runtime','host','timers','group','scheduler')],str(ROOT/'main/pet_p4_event_bus.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
        bins[(enabled,resume)]=binary
    def run(runtime,mode='parse',enabled=1):
        result=subprocess.run([str(bins[(enabled,mode=='resume')]),mode],input=json.dumps(runtime,ensure_ascii=True),text=True,capture_output=True)
        assert result.returncode==0 and not result.stderr,result.stderr;return json.loads(result.stdout)
    return run
@pytest.fixture(scope='module')
def authoring():
    spec=importlib.util.spec_from_file_location('component_program',SKILL/'scripts/component_program.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def test_shared_program_descriptor_and_ownership(native,authoring):
    r=SUITE['runtime'];assert native(r)['enabled'];assert authoring.errors(r)==[]
    for patch in SUITE['reject']:
        bad=patched(r,patch);assert not native(bad)['ok'],patch;assert authoring.errors(bad),patch
    for field in ('image','outputs','actions','timers'):
        bad=copy.deepcopy(r);del bad['program'][field];assert not native(bad)['ok'];assert authoring.errors(bad)
def test_ordinary_firmware_is_closed_and_old_components_unchanged(native,authoring):
    assert not native(SUITE['runtime'],enabled=0)['ok']
    old=copy.deepcopy(SUITE['runtime']);del old['program'];assert native(old,enabled=0)=={'ok':True,'enabled':False};assert authoring.errors(old)==[]
def test_full_mixed_form_navigation_program_contract_without_shared_writers(native,authoring):
    mixed=json.loads((SKILL/'contracts/component-program-form-v1.json').read_text())
    r=mixed['runtime'];assert native(r)['ok'] and not authoring.errors(r)
    sys.path.insert(0,str(SKILL/'scripts'))
    from validate_generated_widget import validate_runtime,DEFAULT_CAPABILITIES
    from app_form import errors as form_errors,structural_disabled
    from bounded_app import smoke
    assert not form_errors(r,mixed['buttons'])
    caps={**DEFAULT_CAPABILITIES,'widgetProgram':'p4-component-program-v1','widgetAppFormV2':'p4-app-form-v2','widgetAppNavigation':'p4-app-navigation-v1'}
    errors=[];validate_runtime(r,'tool',Path('/nonexistent-test-assets'),caps,errors);assert not errors,errors
    report=smoke(r,mixed['buttons']);assert report['ok'],report
    assert report['disabledActions']==structural_disabled(r)=={'field.prev':'single_field','field.next':'single_field'}
    for name in ('focus','editing','keyIndex'):
        bad=copy.deepcopy(r);bad['program']['outputs'][0]=name
        assert not native(bad)['ok'] and authoring.errors(bad)
def test_real_session_commits_timers_stops_ranges_and_retains_state(native):
    assert native(SUITE['runtime'],mode='session')['ok']

def test_optin_retired_snapshot_runs_complete_original_session_suite_and_new_alias_revision_image_guards(native):
    assert native(SUITE['runtime'],mode='resume')['ok']

def test_explicit_ram_resume_uses_real_retired_session_and_never_revives_stopped_background(tmp_path):
    from p4_render_trace_test import function
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    backend=(ROOT/'tests/p4_component_program_test.c').read_text().split('#if PET_COMPONENT_PROGRAM_ENABLED',1)[1].split('static pet_component_program_config_t config;',1)[0]
    code=r'''
#include "pet_p4_component_program_session.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
'''+backend+r'''
typedef struct {int32_t int_value;} value_t;
typedef struct {bool active,media;unsigned save_version;char widget_id[48];value_t vars[8];
 pet_component_program_config_t program;pet_program_snapshot_t program_seed;
 bool program_seed_ready,program_needs_start,program_suspended,program_background_owned;
 uint64_t program_content[3];} miniapp_runtime_t;
typedef struct {char widget_id[48];unsigned resource_state;uint32_t widget_checksum,buttons_checksum,sprites_checksum;} item_t;
typedef struct {unsigned slot;} pet_program_registration_t;
static miniapp_runtime_t g_runtime;
static item_t g_catalog[2]={{"owned-check",1,123,456,0},{"other-check",1,123,456,0}};
static size_t g_catalog_count=2,g_catalog_selected=1;
#define g_component_program session
static bool g_component_program_initialized=true;
static uint32_t g_image_revision=7;
static unsigned model_depth,parent_depth,refreshed;
static bool registered,background_stopped;
static void miniapp_runtime_lock(void){assert(!model_depth&&!parent_depth);model_depth++;}
static void miniapp_runtime_unlock(void){assert(model_depth==1&&!parent_depth);model_depth--;}
static bool component_parent_enter(void){assert(model_depth==1&&!parent_depth);parent_depth++;return true;}
static void component_parent_leave(bool *held){if(*held){assert(parent_depth==1);parent_depth--;}}
static bool background_current_locked(const miniapp_runtime_t *r,pet_program_registration_t*h){(void)r;(void)h;assert(model_depth==1&&!parent_depth);return registered;}
static bool background_project_locked(miniapp_runtime_t*r){assert(model_depth==1&&!parent_depth&&registered);r->program_suspended=background_stopped;return true;}
static void refresh_view(miniapp_runtime_t*r){assert(r==&g_runtime&&model_depth==1);refreshed++;}
'''+function(text,'component_program_apply_seed')+'\n'+function(text,'resume_component_program_locked')+'\n'+function(text,'resume_catalog_index')+r'''
int main(void){
 const char *definition=DEFINITION;
 cJSON *root=cJSON_Parse(definition);assert(root);
 assert(pet_component_program_parse(root,&g_runtime.program));cJSON_Delete(root);
 g_runtime.active=true;strcpy(g_runtime.widget_id,"owned-check");
 g_runtime.program_content[0]=123;g_runtime.program_content[1]=456;
 g_runtime.program_seed=(pet_program_snapshot_t){.schema=g_runtime.program.image.schema,.cells=g_runtime.program.image.cells};
 g_runtime.program_seed_ready=true;
 assert(pet_component_program_session_init(&session,&backend));
 assert(pet_component_program_session_start(&session,&g_runtime.program,&g_runtime.program_seed,7,0)==PET_RUNTIME_OK);
 uint64_t original_generation=session.generation;
 assert(resume_catalog_index(0)&&session.live&&held&&session.generation==original_generation&&!refreshed);
 assert(!resume_catalog_index(1)&&g_catalog_selected==0);
 g_runtime.program_content[0]++;assert(!resume_catalog_index(0));g_runtime.program_content[0]--;
 int32_t base[8]={0,2},output[8];pet_program_event_t event;
 assert(pet_component_program_event(&g_runtime.program,"add",base,&event));
 assert(pet_component_program_session_enqueue(&session,7,&event)==PET_RUNTIME_OK);
 for(unsigned i=0;i<100&&!session.report_valid;i++)if(pet_component_program_session_step(&session,7,1,base,output))memcpy(base,output,sizeof(base));
 assert(base[0]==2&&g_runtime.program_seed.outputs[0]==0); /* Latest commit not old UI seed. */
 assert(pet_component_program_session_stop(&session)==PET_RUNTIME_OK&&!held);
 g_runtime.program_suspended=true;
 assert(resume_catalog_index(0)&&g_runtime.program_needs_start&&!g_runtime.program_suspended);
 assert(g_runtime.vars[0].int_value==2&&g_runtime.program_seed.outputs[0]==2);
 assert(!held&&!live&&!session.live&&session.generation==original_generation&&refreshed==1);
 // Preparation cannot acquire or bypass the real audio/broker admission.
 audio=true;assert(pet_component_program_session_start(&session,&g_runtime.program,&g_runtime.program_seed,7,100)==PET_RUNTIME_DENIED);audio=false;
 assert(!held&&!session.live);g_runtime.program_needs_start=false;
 g_component_program_initialized=false;assert(!resume_catalog_index(0));g_component_program_initialized=true;
 g_image_revision++;assert(!resume_catalog_index(0));g_image_revision--;
 g_runtime.program_background_owned=true;registered=false;assert(!resume_catalog_index(0));
 registered=true;background_stopped=true;assert(resume_catalog_index(0));
 assert(g_runtime.program_suspended&&!g_runtime.program_needs_start&&!session.live&&!held);
 background_stopped=false;assert(resume_catalog_index(0)&&!g_runtime.program_suspended);
 assert(!session.live&&!held&&!model_depth&&!parent_depth);return 0;
}
'''
    code=code.replace('DEFINITION',json.dumps(json.dumps(SUITE['runtime'],ensure_ascii=True)))
    path=tmp_path/'ram-resume.c';path.write_text(code);binary=tmp_path/'ram-resume';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
        '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1','-DPET_COMPONENT_PROGRAM_RAM_RESUME=1',
        '-I',str(ROOT/'main'),'-I',str(vendor),str(path),str(ROOT/'main/pet_p4_component_program.c'),
        str(ROOT/'main/pet_p4_component_program_session.c'),
        *[str(ROOT/'main'/f'pet_p4_program_{name}.c') for name in ('core','image','runtime','host','timers','group','scheduler')],
        str(ROOT/'main/pet_p4_event_bus.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
def test_form_writers_and_independent_feedback_inputs_have_explicit_ownership(native,authoring):
    for key in ('focus_var','active_var'):
        bad=copy.deepcopy(SUITE['runtime']);bad['form']={key:'count'};assert not native(bad)['ok'];assert authoring.errors(bad)
    for key in ('var','draft_var'):
        bad=copy.deepcopy(SUITE['runtime']);bad['form']={'fields':[{key:'count'}]};assert not native(bad)['ok'];assert authoring.errors(bad)
    for key in ('index_var','label_var'):
        bad=copy.deepcopy(SUITE['runtime']);bad['form']={'keyboard':{key:'count'}};assert not native(bad)['ok'];assert authoring.errors(bad)
    bad=copy.deepcopy(SUITE['runtime']);bad['form']={'actions':{'accept':'add'}};assert not native(bad)['ok'];assert authoring.errors(bad)
    r=copy.deepcopy(SUITE['runtime']);r['program']['outputs'][1]='step';assert native(r)['ok'];assert authoring.errors(r)==[]
def test_full_runtime_validator_requires_actual_capability_and_never_grants_from_defaults():
    sys.path.insert(0,str(SKILL/'scripts'));from validate_generated_widget import validate_runtime,DEFAULT_CAPABILITIES
    r=copy.deepcopy(SUITE['runtime']);caps={**DEFAULT_CAPABILITIES};errors=[]
    validate_runtime(r,'tool',Path('/nonexistent-test-assets'),caps,errors);assert any('受限组件程序' in e for e in errors)
    caps['widgetProgram']='p4-component-program-v1';errors=[];validate_runtime(r,'tool',Path('/nonexistent-test-assets'),caps,errors);assert not errors,errors

def test_authoring_routes_programs_before_other_application_simulators():
    sys.path.insert(0,str(SKILL/'scripts'));from bounded_app import smoke
    before=copy.deepcopy(SUITE);report=smoke(SUITE['runtime'],SUITE['buttons'])
    assert report['ok'] and report['mode']=='shared-native-component-program' and not report['deviceVerified'],report
    assert SUITE==before

def test_program_pumps_only_in_serialized_main_turn_not_action_dispatch():
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    assert text.count('component_program_process_locked(now_ms,visible);')==1
    process=text.split('void pet_p4_miniapp_process(uint64_t now_ms, bool visible) {',1)[1].split('bool pet_p4_miniapp_active(',1)[0]
    assert 'miniapp_runtime_lock();' in process.split('component_program_process_locked(now_ms,visible);')[0]
    assert 'component_program_process_locked(now_ms,visible);' in process
    assert 'component_program_process_locked(' not in text.split('static bool dispatch_action_observed(',1)[1].split('void pet_p4_miniapp_process(',1)[0]

def test_actual_component_parent_lifecycle_input_checkpoint_and_quiesced_replacement(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    body=text.split('static void component_program_values(',1)[1].split('#endif\n/* Per-call PSRAM workspace:',1)[0]
    from p4_render_trace_test import function
    helpers=function(text,'component_program_snapshot_locked')+'\nstatic void component_program_values('+body
    status='bool pet_p4_miniapp_program_status(pet_p4_miniapp_program_status_t *out){'+text.split('bool pet_p4_miniapp_program_status(pet_p4_miniapp_program_status_t *out){',1)[1].split('bool pet_p4_miniapp_slicing_active(',1)[0]
    restore=function(text,'restore_component_runtime')
    backend=(ROOT/'tests/p4_component_program_test.c').read_text().split('#if PET_COMPONENT_PROGRAM_ENABLED',1)[1].split('static pet_component_program_config_t config;',1)[0]
    definition=json.dumps(json.dumps(SUITE['runtime'],ensure_ascii=True))
    code=r'''
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_program_persistence.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
'''+backend+r'''
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static void *heap_caps_malloc(size_t bytes,int caps){assert(caps==3);return malloc(bytes);}
static void heap_caps_free(void *p){free(p);}
#define MINIAPP_VAR_INT 1
typedef struct {int type;int32_t int_value;} test_var_t;
typedef struct {bool active,slicing;unsigned save_version,var_count;char widget_id[48];test_var_t vars[8];
 pet_component_program_config_t program;pet_program_snapshot_t program_seed;bool program_seed_ready,program_needs_start,program_suspended;} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static pet_component_program_session_t g_component_program;
static bool g_component_program_initialized;
static pet_program_scheduler_t g_component_program_scheduler;
static bool g_component_program_scheduler_initialized;
static uint32_t g_component_program_last_us,g_component_program_max_us;
static uint32_t g_component_program_attempt_revision;
static pet_program_runtime_result_t g_component_program_start_result;
typedef struct {uint8_t phase,result;bool live,retiring,report_valid,slice_stalled;uint32_t last_step_us,max_step_us,revision;uint16_t used_fuel,slice_budget_us;} pet_p4_miniapp_program_status_t;
static uint32_t g_image_revision=1,g_viewport_epoch,refreshes;
static bool locked,checkpoint_allowed=true;static unsigned writes,loads;
static pet_program_save_result_t load_status=PET_PROGRAM_SAVE_EMPTY;
static pet_program_snapshot_t stored;
static struct {bool occupied;} g_storage_lane;
static pet_resource_lease_t g_storage_resource_lease;
static void miniapp_runtime_lock(void){assert(!locked);locked=true;}
static void miniapp_runtime_unlock(void){assert(locked);locked=false;}
static int64_t esp_timer_get_time(void){static int64_t now;return ++now;}
static void copy_utf8(char *out,size_t length,const char *text){snprintf(out,length,"%s",text);}
static void refresh_view(miniapp_runtime_t *r){(void)r;assert(locked);refreshes++;}
static void component_program_retire_locked(void){assert(locked);if(g_component_program_initialized&&(g_component_program.live||g_component_program.retiring))(void)pet_component_program_session_stop(&g_component_program);}
bool pet_program_host_device_init(pet_program_host_t *h){return pet_program_host_init(h,&backend);}
bool pet_program_persistence_enqueue(const char *id,const pet_program_snapshot_t *s){assert(!locked&&!strcmp(id,"component"));if(!checkpoint_allowed)return false;stored=*s;writes++;return true;}
pet_program_save_result_t pet_program_persistence_load(const char *id,const pet_program_image_t *image,pet_program_snapshot_t *seed){(void)image;assert(!locked&&id[0]);loads++;if(load_status==PET_PROGRAM_SAVE_VALUE)*seed=stored;return load_status;}
pet_program_save_result_t pet_program_persistence_load_with_lease(const char *id,const pet_program_image_t *image,pet_program_snapshot_t *seed,pet_resource_lease_t value){(void)value;return pet_program_persistence_load(id,image,seed);}
static bool restore_component_runtime_from(miniapp_runtime_t *r,bool cached){(void)r;(void)cached;return true;}
pet_app_store_status_t pet_app_store_load(const char *id,char s[PET_APP_SAVE_BYTES]){(void)id;(void)s;assert(!"unrelated app save must not be loaded");return PET_APP_STORE_INVALID;}
pet_app_store_status_t pet_app_store_load_with_lease(const char *id,char s[PET_APP_SAVE_BYTES],pet_resource_lease_t value){(void)value;return pet_app_store_load(id,s);}
'''+helpers+restore+status+'\nstatic const char *fixture='+definition+r''';
int main(void){
 cJSON *root=cJSON_Parse(fixture);assert(root&&pet_component_program_parse(root,&g_runtime.program));cJSON_Delete(root);
 strcpy(g_runtime.widget_id,"component");g_runtime.active=true;g_runtime.var_count=2;
 g_runtime.vars[0]=(test_var_t){MINIAPP_VAR_INT,0};g_runtime.vars[1]=(test_var_t){MINIAPP_VAR_INT,2};
 g_runtime.program_seed.schema=1;g_runtime.program_seed.cells=1;g_runtime.program_seed_ready=g_runtime.program_needs_start=true;
 // Actual install/cold activation yields after new-runtime publication but
 // BEFORE the outer command publishes its new page. Old hidden visibility
 // must neither stop the pending lifecycle nor admit it prematurely.
 g_storage_lane.occupied=true;admitted=true;
 miniapp_runtime_lock();component_program_process_locked(0,false);component_program_process_locked(0,true);miniapp_runtime_unlock();
 assert(g_runtime.program_needs_start&&!g_runtime.program_suspended&&!g_component_program.live&&!held);
 g_storage_lane.occupied=false;
 admitted=false;miniapp_runtime_lock();component_program_process_locked(0,true);miniapp_runtime_unlock();assert(!g_component_program.live&&!held);
 pet_p4_miniapp_program_status_t d;assert(pet_p4_miniapp_program_status(&d)&&!d.live&&!d.report_valid&&d.phase==PET_RUNTIME_DENIED);
 admitted=true;miniapp_runtime_lock();component_program_process_locked(1,true);miniapp_runtime_unlock();assert(!g_component_program.live); // admission recovery is not restart
 g_image_revision++;g_runtime.program_needs_start=true;
 miniapp_runtime_lock();component_program_process_locked(2,true);miniapp_runtime_unlock();assert(g_component_program.live&&held);
 int32_t values[8]={0,2};pet_program_event_t event;assert(pet_component_program_event(&g_runtime.program,"add",values,&event));
 g_storage_lane.occupied=true; // unrelated storage must not stall an existing live owner
 miniapp_runtime_lock();assert(pet_component_program_session_enqueue(&g_component_program,g_image_revision,&event)==PET_RUNTIME_OK);miniapp_runtime_unlock();
 for(unsigned i=0;i<3;i++){miniapp_runtime_lock();component_program_process_locked(3+i,true);miniapp_runtime_unlock();}
 assert(g_runtime.vars[0].int_value==2&&g_runtime.vars[1].int_value==2&&refreshes==1);
 g_storage_lane.occupied=false;
 assert(pet_p4_miniapp_program_status(&d)&&d.live&&d.report_valid&&d.revision==1);
 checkpoint_allowed=false;assert(!save_component_program()&&writes==0);checkpoint_allowed=true;assert(save_component_program()&&writes==1&&stored.values[0]==2);
 // The real preparation function takes the latest live state and quiesces
 // that owner before unlocking for package work; it does not use stale disk.
 miniapp_runtime_t next=g_runtime;next.program_seed.values[0]=-99;loads=0;
 assert(restore_component_runtime(&next)&&!g_component_program.live&&!held&&loads==0&&next.program_seed.values[0]==2);
 assert(g_runtime.program_suspended&&!next.program_suspended);
 miniapp_runtime_lock();component_program_process_locked(100,true);miniapp_runtime_unlock();assert(!g_component_program.live);
 next.program.image.schema=2;assert(!restore_component_runtime(&next)&&g_runtime.vars[0].int_value==2&&stored.values[0]==2);
 next=g_runtime;strcpy(next.widget_id,"other");load_status=PET_PROGRAM_SAVE_CORRUPT;assert(!restore_component_runtime(&next)&&stored.values[0]==2);
 miniapp_runtime_lock();component_program_process_locked(200,false);component_program_process_locked(300,true);miniapp_runtime_unlock();
 assert(!g_component_program.live&&g_runtime.vars[0].int_value==2&&g_runtime.program_suspended);
 // Visibility recovery cannot restart the foreground owner or its automation.
 g_runtime.program_needs_start=true;
 miniapp_runtime_lock();component_program_process_locked(350,true);miniapp_runtime_unlock();assert(!g_component_program.live);
 g_image_revision++;g_runtime.program_needs_start=true;
 g_runtime.program_suspended=false; // New explicit activation, not old visibility recovery.
 assert(pet_p4_miniapp_program_status(&d)&&!d.live&&!d.report_valid&&!d.max_step_us&&d.phase==PET_RUNTIME_IDLE);
 admitted=false;miniapp_runtime_lock();component_program_process_locked(400,true);miniapp_runtime_unlock();
 assert(pet_p4_miniapp_program_status(&d)&&!d.live&&!d.report_valid&&d.phase==PET_RUNTIME_DENIED);
 // A real hide still retires an already-started owner even during storage.
 g_image_revision++;g_runtime.program_needs_start=true;admitted=true;
 miniapp_runtime_lock();component_program_process_locked(410,true);miniapp_runtime_unlock();assert(g_component_program.live&&held);
 g_storage_lane.occupied=true;
 miniapp_runtime_lock();component_program_process_locked(420,false);miniapp_runtime_unlock();
 assert(!g_component_program.live&&!held&&g_runtime.program_suspended);
 g_storage_lane.occupied=false;
 miniapp_runtime_lock();component_program_process_locked(430,true);miniapp_runtime_unlock();assert(!g_component_program.live&&!held);return 0;
}
'''
    source=tmp_path/'parent.c';source.write_text(code);binary=tmp_path/'parent';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-DPET_COMPONENT_PROGRAM_ENABLED=1','-I',str(ROOT/'main'),'-I',str(vendor),str(source),
        str(ROOT/'main/pet_p4_component_program.c'),str(ROOT/'main/pet_p4_component_program_session.c'),
        *[str(ROOT/'main'/f'pet_p4_program_{n}.c') for n in ('core','image','runtime','host','timers','group','scheduler')],str(ROOT/'main/pet_p4_event_bus.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True);assert result.returncode==0 and not result.stderr,result.stderr
