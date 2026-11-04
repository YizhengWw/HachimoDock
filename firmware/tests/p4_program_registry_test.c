/* Native fixed registry using REAL factory/broker/runtime/scheduler. Only
 * preloaded RAM save pins/checkpoint adapter and clock/audio flags are mocked.
 * No filesystem, physical audio, USB, arbitrary quota, or durability claim. */
#include "pet_p4_program_registry.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static pet_program_registry_t registry;
static int64_t clock_us=1;
static bool checkpoint_ok=true;
static unsigned checkpoints;
static pet_program_snapshot_t saved;
static uint64_t pin_next;
static const void *pinned[2];
static uint64_t pin_serial[2];
int64_t esp_timer_get_time(void){return clock_us++;}
bool pet_p4_audio_active(void){return false;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
bool pet_app_store_pin_scoped(uint32_t domain,const char *id,const void *owner,pet_app_store_pin_t *out){
    assert(domain==PET_PROGRAM_SAVE_DOMAIN&&owner&&out&&!out->serial);
    unsigned slot=!strcmp(id,"alpha")?0:!strcmp(id,"beta")?1:2;
    if(slot>=2||pinned[slot])return false;
    pinned[slot]=owner;pin_serial[slot]=++pin_next;
    *out=(pet_app_store_pin_t){owner,pin_serial[slot],(uint8_t)slot};return true;
}
bool pet_app_store_unpin(pet_app_store_pin_t *pin){
    if(!pin||pin->slot>=2||!pin->serial||pin_serial[pin->slot]!=pin->serial||pinned[pin->slot]!=pin->owner)return false;
    pinned[pin->slot]=NULL;pin_serial[pin->slot]=0;*pin=(pet_app_store_pin_t){0};return true;
}
bool pet_program_persistence_enqueue(const char *id,const pet_program_snapshot_t *s){
    assert(!strcmp(id,"alpha")&&pinned[0]);if(!checkpoint_ok)return false;saved=*s;checkpoints++;return true;
}
static pet_component_program_config_t config;
static pet_program_snapshot_t seed;
static int32_t base[8]={0,7},values[8];
static uint64_t now_ms;
static void turn(void){
    assert(pet_program_registry_step(&registry,++now_ms)==PET_SCHED_TURN);
#if PET_PROGRAM_PHASE_TRACE
    const pet_program_phase_t *p=&registry.last_phase;
    assert(!p->invalid_clock&&p->end_us>=p->start_us&&p->parent_us==p->end_us-p->start_us);
    assert(p->scheduler_us>0&&p->collected==1&&p->collect_us>0&&p->capture_us>0);
    assert(p->parent_us>=p->scheduler_us+p->collect_us+p->capture_us);
#endif
}
static int32_t count(pet_program_registration_t h){
    pet_program_snapshot_t s;
    assert(pet_program_registry_view(&registry,h,base,values,&s));assert(values[1]==7);return values[0];
}
static void finish(pet_program_registration_t h,int32_t expected){
    unsigned turns=0;while(count(h)!=expected&&turns++<12)turn();assert(count(h)==expected);
}
static void empty_broker(void){pet_resource_usage_t u;pet_resources_device_usage(&u);assert(!u.live_owners&&!u.revoked_owners&&!u.leases[PET_RESOURCE_PROGRAM]);}
int main(void){
    assert(pet_program_registry_init(&registry,pet_program_host_device_init));
    assert(!pet_program_registry_init(&registry,pet_program_host_device_init));
    assert(pet_resources_limit(PET_RESOURCE_PROGRAM).leases==1); // No fake multi-owner quota.
    config.enabled=true;config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=128,.cells=1,.valid=true};
    const pet_program_insn_t code[]={
      {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
      {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
    memcpy(config.image.code,code,sizeof(code));
    config.projection=(pet_program_ui_projection_t){.schema=1,.variables=2,.minimum={0,0},.maximum={100,100}};
    memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(config.projection.target));config.projection.target[0]=0;
    strcpy(config.actions[0].name,"add");config.actions[0].event=(pet_program_event_t){1,{2}};
    memset(config.actions[0].input,-1,sizeof(config.actions[0].input));config.action_count=1;
    config.timer_count=1;config.timers[0]=(pet_program_timer_spec_t){.id=1,.delay_ms=100,.interval_ms=100,.event={1,{1}}};
    seed.schema=1;seed.cells=1;const uint64_t content[3]={11,22,33};
    pet_program_registration_t h={0};
    // Malformed native staging must not index beyond projection arrays even
    // before the host validates the full owned image/contract.
    pet_component_program_config_t malformed=config;malformed.projection.variables=255;malformed.projection.target[0]=8;
    assert(pet_program_registry_attach(&registry,"alpha",content,&malformed,&seed,base,true,0,&h)==PET_REGISTRY_INVALID&&!h.serial);
    malformed=config;malformed.image.cells=255;
    assert(pet_program_registry_attach(&registry,"alpha",content,&malformed,&seed,base,true,0,&h)==PET_REGISTRY_INVALID&&!h.serial);
    malformed=config;malformed.action_count=255;
    assert(pet_program_registry_attach(&registry,"alpha",content,&malformed,&seed,base,true,0,&h)==PET_REGISTRY_INVALID&&!h.serial);
    pet_program_event_t invalid_event;
    assert(!pet_component_program_event(&malformed,"add",base,&invalid_event));
    malformed=config;malformed.actions[0].event.count=255;
    assert(!pet_component_program_event(&malformed,"add",base,&invalid_event));
    assert(pet_program_registry_attach(&registry,"missing",content,&config,&seed,base,true,0,&h)==PET_REGISTRY_DENIED&&!h.serial);
    assert(pet_program_registry_attach(&registry,"alpha",content,&config,&seed,base,true,0,&h)==PET_REGISTRY_OK);
    assert(h.registry==&registry&&h.serial&&pinned[0]);
    pet_program_registry_t moved=registry;
    assert(pet_program_registry_step(&moved,0)==PET_SCHED_INVALID);
    assert(pet_program_registry_stop(&moved,h)==PET_RUNTIME_STALE);
    assert(!pet_program_registry_release(&moved,h));
    assert(!pet_program_registry_release(&registry,h)); // Live owners cannot be dropped.
    pet_program_registration_t other={0};
    assert(pet_program_registry_attach(&registry,"beta",content,&config,&seed,base,true,0,&other)==PET_REGISTRY_DENIED&&!other.serial&&!pinned[1]);
    // Hide/switch is a parent policy; registry timer survives WITHOUT needing
    // a visible UI model. One shared operation each real turn, not four pumps.
    now_ms=99;finish(h,1);assert(registry.entries[h.slot].session.timers.accepted==1);
    assert(pet_program_registry_enqueue(&registry,h,"add",base)==PET_RUNTIME_OK);finish(h,3);
    pet_program_registration_t same={0};
    assert(pet_program_registry_attach(&registry,"alpha",content,&config,&seed,base,true,now_ms,&same)==PET_REGISTRY_ATTACHED);
    assert(same.serial==h.serial&&count(h)==3&&registry.entries[h.slot].session.timers.accepted==1);
    uint64_t changed[3]={11,22,34};pet_program_registration_t refused={0};
    assert(pet_program_registry_attach(&registry,"alpha",changed,&config,&seed,base,true,now_ms,&refused)==PET_REGISTRY_CONFLICT&&!refused.serial);
    assert(pet_program_registry_attach(&registry,"alpha",content,&config,&seed,base,false,now_ms,&refused)==PET_REGISTRY_CONFLICT);
    checkpoint_ok=false;assert(!pet_program_registry_checkpoint_one(&registry)&&!checkpoints&&pinned[0]);
    assert(pet_program_registry_stop(&registry,h)==PET_RUNTIME_OK);
    assert(!pet_program_registry_release(&registry,h)); // Latest commit not yet accepted by RAM save.
    assert(pet_program_registry_enqueue(&registry,h,"add",base)==PET_RUNTIME_STOPPED);
    assert(pet_program_registry_attach(&registry,"alpha",content,&config,&seed,base,true,now_ms,&refused)==PET_REGISTRY_STOPPED);
    checkpoint_ok=true;assert(pet_program_registry_checkpoint_one(&registry)&&checkpoints==1&&saved.values[0]==3);
    for(unsigned i=0;i<4;i++)assert(pet_program_registry_checkpoint_one(&registry));
    assert(checkpoints==1); // Accepted RAM checkpoint is not automatically resubmitted.
    assert(pet_program_registry_release(&registry,h)&&!pinned[0]);empty_broker();
    pet_program_registration_t next={0};
    assert(pet_program_registry_attach(&registry,"alpha",content,&config,&saved,base,true,now_ms,&next)==PET_REGISTRY_OK);
    assert(next.serial>h.serial&&count(next)==3);
    assert(pet_program_registry_stop(&registry,h)==PET_RUNTIME_STALE&&!pet_program_registry_release(&registry,h));
    assert(pet_program_registry_enqueue(&registry,next,"add",base)==PET_RUNTIME_OK);
    // Force the physical commit/observer interleaving: actual parent commits,
    // then real audio reservation wins BEFORE registry collects/project checks.
    turn();turn();pet_program_schedule_diagnostic_t d={0};
    assert(pet_program_scheduler_step(&registry.scheduler,++now_ms,&d)==PET_SCHED_TURN);
    assert(registry.entries[next.slot].session.host.runtime.state.cells[0]==5);
    pet_resource_realtime_t realtime={0};assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
    (void)pet_program_registry_step(&registry,++now_ms);
    assert(count(next)==5&&registry.entries[next.slot].stopped);
    assert(pet_program_registry_checkpoint_one(&registry)&&saved.values[0]==5);
    pet_resources_device_realtime_end(&realtime);
    for(unsigned n=0;n<10;n++)(void)pet_program_registry_step(&registry,++now_ms);
    assert(count(next)==5); // No timer catch-up or automatic recovery.
    assert(pet_program_registry_stop(&registry,next)==PET_RUNTIME_OK);
    assert(pet_program_registry_release(&registry,next));empty_broker();
    pet_program_registration_t exhausted={0};registry.next_serial=UINT64_MAX;
    assert(pet_program_registry_attach(&registry,"alpha",content,&config,&saved,base,true,now_ms,&exhausted)==PET_REGISTRY_EXHAUSTED&&!exhausted.serial&&!pinned[0]);
    puts("PASS real one-lease registry ownership/checkpoint/revocation; no installed/background hardware acceptance claimed");
    return 0;
}
