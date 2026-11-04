"""Real factory/broker conditional admission; no fake lease, device or sound."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_only_private_foreground_profile_opens_reviewed_admission():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    private='env:esp32_p4_program_candidate'
    assert config[private]['extends']=='env:esp32_p4_canvas_candidate'
    public='env:esp32_p4_program_public_candidate'
    assert config[public]['extends']=='env:esp32_p4_capability_parent' and 'PET_P4_PUBLIC_PROGRAM_PROFILE=1' in config[public]['build_flags']
    for name in config.sections():
        if 'PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1' in config[name].get('build_flags',''):
            assert name in (private,public)
    assert 'EXECUTION_REVIEWED' not in config['env:esp32_p4_evboard']['build_flags']
    assert 'EXECUTION_REVIEWED' not in config['env:esp32_p4_canvas_candidate']['build_flags']

def test_real_program_factory_admits_once_revokes_before_flags_and_preserves_commit(tmp_path):
    code=r'''
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
static int64_t clock_us=1;
int64_t esp_timer_get_time(void){return clock_us++;}
bool pet_p4_audio_active(void){return false;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
static pet_component_program_session_t first,other;
static pet_program_scheduler_t parent;
static pet_component_program_config_t config;
static pet_program_snapshot_t seed;
static int32_t base[8],out[8];
static bool turn(uint64_t revision){
 pet_program_schedule_diagnostic_t d={0};
 (void)pet_program_scheduler_step(&parent,(uint64_t)clock_us/1000,&d);
 return pet_component_program_session_collect(&first,revision,base,out);
}
static void empty(void){pet_resource_usage_t u;pet_resources_device_usage(&u);
 assert(!u.live_owners&&!u.revoked_owners&&!u.leases[PET_RESOURCE_PROGRAM]);}
int main(void){
 assert(pet_program_scheduler_init(&parent));
 assert(pet_program_host_device_init(&first.host)&&pet_program_host_device_init(&other.host));
 assert(first.host.admission_kind==PET_RESOURCE_PROGRAM);
 assert(pet_component_program_session_bind_scheduler(&first,&parent));
 assert(pet_component_program_session_bind_scheduler(&other,&parent));
 config.enabled=true;config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=128,.cells=1,.valid=true};
 const pet_program_insn_t code[]={
  {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
  {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
 memcpy(config.image.code,code,sizeof(code));
 config.projection=(pet_program_ui_projection_t){.schema=1,.variables=1,.minimum={0},.maximum={100}};
 memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(config.projection.target));config.projection.target[0]=0;
 seed.schema=1;seed.cells=1;
 pet_program_runtime_result_t started=pet_component_program_session_start(&first,&config,&seed,1,0);
#if PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED
 assert(started==PET_RUNTIME_OK&&first.host.lease.kind==PET_RESOURCE_PROGRAM);
 assert(pet_resources_limit(PET_RESOURCE_PROGRAM).leases==1);
 assert(pet_component_program_session_start(&other,&config,&seed,1,0)==PET_RUNTIME_BUSY);
 assert(other.host.last_admission==PET_RESOURCE_BUSY&&!other.host.bound);
 pet_resource_owner_t diagnostic={0};pet_resource_lease_t native={0};
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE),&diagnostic)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(diagnostic,PET_RESOURCE_NATIVE_PROBE,64,&native)==PET_RESOURCE_BUSY&&!native.serial);
 pet_resources_device_revoke(diagnostic);memset(&diagnostic,0,sizeof(diagnostic));
 const pet_program_event_t event={1,{41}};
 assert(pet_component_program_session_enqueue(&first,1,&event)==PET_RUNTIME_OK);
 assert(!turn(1)&&!turn(1)&&turn(1));assert(out[0]==41);
 assert(pet_component_program_session_snapshot(&first,&seed)&&seed.values[0]==41);
 pet_resource_realtime_t audio={0};assert(pet_resources_device_realtime_begin(&audio)==PET_RESOURCE_OK);
 assert(!pet_resources_device_current(first.host.lease));
 /* The four sampled flags stay false: the real reservation alone must win. */
 assert(!turn(1)&&!first.live&&!first.host.bound);empty();
 pet_resources_device_realtime_end(&audio);
 assert(!turn(1)&&pet_component_program_session_enqueue(&first,1,&event)!=PET_RUNTIME_OK);
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_NATIVE_PROBE),&diagnostic)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(diagnostic,PET_RESOURCE_NATIVE_PROBE,64,&native)==PET_RESOURCE_OK);
 pet_resources_device_revoke(diagnostic);
 assert(pet_component_program_session_start(&first,&config,&seed,2,(uint64_t)clock_us/1000)==PET_RUNTIME_BUSY);
 assert(pet_resources_device_finish(native)==PET_RESOURCE_FINISH_REVOKED);
 assert(pet_component_program_session_start(&first,&config,&seed,2,(uint64_t)clock_us/1000)==PET_RUNTIME_OK);
 const pet_program_event_t overflow={1,{INT32_MAX}};
 assert(pet_component_program_session_enqueue(&first,2,&overflow)==PET_RUNTIME_OK);
 assert(!turn(2)&&!turn(2));
 assert(first.last_report.result==PET_PROGRAM_OVERFLOW&&!first.live&&!first.host.bound);
 assert(pet_component_program_session_snapshot(&first,&seed)&&seed.values[0]==41&&seed.outputs[0]==41);empty();
#else
 assert(started==PET_RUNTIME_DENIED&&!first.host.bound);
 assert(!pet_resources_limit(PET_RESOURCE_PROGRAM).leases);empty();
 /* Keep the exact production dispatch/collect route linked in closed builds. */
 assert(!turn(1));
#endif
 return 0;
}
'''
    path=tmp_path/'reviewed.c';path.write_text(code)
    sources=('resources','resources_device','component_program_session','program_core','program_image','program_runtime','program_host','program_host_device','program_group','program_scheduler','program_timers','event_bus')
    for reviewed in (0,1):
        binary=tmp_path/f'profile-{reviewed}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-DPET_COMPONENT_PROGRAM_ENABLED=1',f'-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED={reviewed}',
            '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),str(path),
            *[str(ROOT/'main'/f'pet_p4_{name}.c') for name in sources],'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)
