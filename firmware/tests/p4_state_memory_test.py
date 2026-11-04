"""Task-only model placement and real dispatch acceptance expression. No device.

Actual P4 ELF section, memory fragmentation and performance are separate gates.
"""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('external', [0, 1])
def test_state_model_keeps_layout_address_and_snapshot_semantics(tmp_path, external):
    main = (ROOT/'main/pet_p4_main.c').read_text()
    start = main.index('#ifndef PET_P4_STATE_EXTERNAL\n')
    placement = main[start:main.index('#define PET_P4_PROTOCOL_LINE_BUFFER_BYTES', start)]
    code = r'''
#include "pet_p4_protocol.h"
#include <assert.h>
#include <string.h>
#ifdef __APPLE__
#define EXT_RAM_BSS_ATTR __attribute__((section("__DATA,__extbss")))
#else
#define EXT_RAM_BSS_ATTR __attribute__((section(".ext_ram.bss")))
#endif
'''+placement+r'''
static pet_p4_runtime_state_t snapshot;
int main(void){
 pet_p4_runtime_state_t *original=&g_state;
 memset(&g_state,0,sizeof(g_state));
 strcpy(g_state.screen_page,"app");strcpy(g_state.current_title,"retained");
 g_state.asset_revision=73;g_state.session_queue_count=1;
 strcpy(g_state.session_queue[0].id,"stable-task-model");
 memcpy(&snapshot,&g_state,sizeof(snapshot));
 assert(!memcmp(&snapshot,&g_state,sizeof(snapshot)));
 snapshot.asset_revision++;strcpy(snapshot.screen_page,"main");
 assert(&g_state==original&&g_state.asset_revision==73&&!strcmp(g_state.screen_page,"app"));
 assert(!strcmp(snapshot.session_queue[0].id,"stable-task-model"));
 return 0;
}
'''
    src=tmp_path/'model.c';src.write_text(code);binary=tmp_path/'model'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    f'-DPET_P4_STATE_EXTERNAL={external}','-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


@pytest.mark.parametrize('parent', [0, 1])
@pytest.mark.parametrize('background', [0, 1])
def test_actual_program_dispatch_never_acks_a_failed_enqueue(tmp_path, parent, background):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    dispatch=function(text,'dispatch_action_observed')
    start=dispatch.index('  if(g_runtime.active&&g_runtime.program.enabled){')
    block=dispatch[start:dispatch.index('#endif\n  int form_op',start)]
    # Compile the actual production branch. Only its external seams are mocked;
    # the separate real-session suite covers queue, owner and audio behavior.
    code=r'''
#include "pet_p4_program_runtime.h"
/* Branch-selection mocks only, after production headers load default-off guards. */
#undef PET_COMPONENT_PROGRAM_PARENT_UNIFIED
#undef PET_COMPONENT_BACKGROUND_ENABLED
#define PET_COMPONENT_PROGRAM_PARENT_UNIFIED TEST_DISPATCH_PARENT
#define PET_COMPONENT_BACKGROUND_ENABLED TEST_DISPATCH_BACKGROUND
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int pet_program_event_t;
static struct {bool active;struct {bool enabled;} program;} g_runtime;
static bool g_component_program_initialized,admit=true,has_action=true;
static int g_component_program,lock_depth=1,parent_depth,calls;
static unsigned accepted_stamps;
static uint32_t g_image_revision=17;
static pet_program_runtime_result_t result;
#if PET_COMPONENT_BACKGROUND_ENABLED
static bool bg_handled,bg_accepted;
static bool background_enqueue_locked(const char *action,const int32_t values[8],bool *accepted){(void)action;assert(values[0]==123);*accepted=bg_accepted;return bg_handled;}
#endif
static void miniapp_runtime_unlock(void){assert(lock_depth==1);lock_depth--;}
static void runtime_action_accepted(const void *r,const char *action,uint64_t now){assert(r==&g_runtime&&action&&now==100&&lock_depth==1);accepted_stamps++;}
static void component_program_values(const void *r,int32_t values[8]){assert(r==&g_runtime);values[0]=123;}
static bool pet_component_program_event(const void *p,const char *a,const int32_t values[8],pet_program_event_t *e){(void)p;(void)a;assert(values[0]==123);*e=42;return has_action;}
static pet_program_runtime_result_t pet_component_program_session_enqueue(int *s,uint32_t revision,const pet_program_event_t *event){assert(s==&g_component_program&&revision==17&&*event==42&&lock_depth==1);calls++;return result;}
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
static bool component_parent_enter(void){assert(!parent_depth);if(admit)parent_depth++;return admit;}
static void component_parent_leave(bool *held){if(*held){assert(parent_depth==1);parent_depth--;}}
#endif
static bool run(const char *action){
 uint64_t now_ms=100;
'''+block+r'''
 miniapp_runtime_unlock();return false;
}
int main(void){
 g_runtime.active=g_runtime.program.enabled=true;
 for(int n=PET_RUNTIME_OK;n<=PET_RUNTIME_STOPPED;n++){
   lock_depth=1;g_component_program_initialized=true;result=(pet_program_runtime_result_t)n;
   int before=calls;unsigned stamps=accepted_stamps;assert(run("local.compute")== (n==PET_RUNTIME_OK));
   assert(calls==before+1&&!lock_depth&&!parent_depth);
   assert(accepted_stamps==stamps+(n==PET_RUNTIME_OK));
 }
 g_component_program_initialized=false;lock_depth=1;int before=calls;
 assert(!run("local.compute")&&calls==before&&!lock_depth&&!parent_depth);
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
 admit=false;g_component_program_initialized=true;lock_depth=1;
 assert(!run("local.compute")&&calls==before&&!lock_depth&&!parent_depth);
#else
 (void)admit;
#endif
#if PET_COMPONENT_BACKGROUND_ENABLED
 bg_handled=true;
 for(int n=0;n<2;n++){
   bg_accepted=n;lock_depth=1;
   unsigned stamps=accepted_stamps;assert(run("local.compute")==bg_accepted&&calls==before&&!lock_depth&&!parent_depth);
   assert(accepted_stamps==stamps+(unsigned)bg_accepted);
 }
#endif
 return 0;
}
'''
    # The event typedef is supplied by the production runtime header.
    code=code.replace('typedef int pet_program_event_t;\n','')
    code=code.replace('*e=42','e->count=1;e->words[0]=42').replace('*event==42','event->count==1&&event->words[0]==42')
    src=tmp_path/'dispatch.c';src.write_text(code);binary=tmp_path/'dispatch'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined',f'-DTEST_DISPATCH_PARENT={parent}',f'-DTEST_DISPATCH_BACKGROUND={background}',
                    '-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
