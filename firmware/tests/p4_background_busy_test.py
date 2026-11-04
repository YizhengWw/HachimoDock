"""Production BUSY classifier with the actual resource broker, no device."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def test_busy_requires_other_live_current_owner_and_full_real_broker(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp_background.inc').read_text()
    helper=function(text,'background_other_program_busy')
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_resources.h"
#define PET_PROGRAM_REGISTRY_SLOTS 4
typedef struct {char id[48];bool occupied,stopped;struct{bool live;struct{pet_resource_lease_t lease;}host;}session;}pet_program_registry_entry_t;
static struct{pet_program_registry_entry_t entries[4];}g_background_programs;
static pet_resource_broker_t broker;
static void pet_resources_device_usage(pet_resource_usage_t*out){pet_resources_usage(&broker,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
'''+helper+r'''
int main(void){
 assert(!background_other_program_busy("second"));
 pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
 assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM),&owner)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,owner,PET_RESOURCE_PROGRAM,4096,&lease)==PET_RESOURCE_OK);
 pet_program_registry_entry_t*e=&g_background_programs.entries[0];strcpy(e->id,"first");e->occupied=e->session.live=true;e->session.host.lease=lease;
 pet_resource_broker_t before=broker;assert(background_other_program_busy("second"));assert(!memcmp(&broker,&before,sizeof broker));
 assert(!background_other_program_busy("first"));
 e->stopped=true;assert(!background_other_program_busy("second"));e->stopped=false;
 e->session.live=false;assert(!background_other_program_busy("second"));e->session.live=true;
 assert(pet_resources_owner_revoke(&broker,owner));assert(!background_other_program_busy("second"));
 (void)pet_resources_finish(&broker,lease);assert(!background_other_program_busy("second"));
 return 0;
}
'''
    source=tmp_path/'busy.c';source.write_text(code);exe=tmp_path/'busy'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
                    '-I',str(ROOT/'main'),str(source),str(ROOT/'main/pet_p4_resources.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)


def test_busy_refusal_precedes_checkpoint_and_retirement():
    start=function((ROOT/'main/pet_p4_miniapp_background.inc').read_text(),'pet_p4_miniapp_background_start')
    assert start.index('if(duplicate)') < start.index('if(program_busy)') < start.index('if(found&&!background_prepare_remove')
    assert start.index('"PROGRAM_BUSY"') < start.index('save_app_runtime()')
