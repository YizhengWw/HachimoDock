#include "pet_p4_component_program_session.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if PET_COMPONENT_PROGRAM_ENABLED
static pet_resource_broker_t broker;
static uint64_t generation,serial;
static bool admitted=true,live,held,audio;
static pet_resource_lease_t lease;
static pet_component_program_session_t session;
static bool stop_on_admission;
static bool stop_on_schedule_admission;
static unsigned admission_stop_count;
static bool busy(void *ctx){
  (void)ctx;
  if(stop_on_admission){
    stop_on_admission=false;
    assert(pet_component_program_session_stop(&session)==PET_RUNTIME_BUSY);
    admission_stop_count++;
  }
  if(stop_on_schedule_admission&&session.group.instance&&session.group.inside){
    stop_on_schedule_admission=false;
    assert(pet_component_program_session_stop(&session)==PET_RUNTIME_BUSY);
    admission_stop_count++;
  }
  return audio;
}
static pet_resource_result_t owner(void *ctx,uint32_t mask,pet_resource_owner_t *out){
  (void)ctx;assert(mask==PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM));
  if(!admitted)return PET_RESOURCE_UNAVAILABLE;
  assert(!live&&!held);*out=(pet_resource_owner_t){&broker,++generation,0};live=true;return PET_RESOURCE_OK;
}
static pet_resource_result_t acquire(void *ctx,pet_resource_owner_t own,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out){
  (void)ctx;assert(own.generation==generation&&live&&!held);lease=(pet_resource_lease_t){own,++serial,units,kind,0};*out=lease;held=true;return PET_RESOURCE_OK;
}
static bool current(void *ctx,pet_resource_lease_t value){(void)ctx;return live&&held&&value.serial==lease.serial&&value.owner.generation==generation;}
static void revoke(void *ctx,pet_resource_owner_t own){(void)ctx;assert(own.generation==generation);live=false;}
static pet_resource_finish_t finish(void *ctx,pet_resource_lease_t value){(void)ctx;assert(held&&value.serial==lease.serial);held=false;return PET_RESOURCE_FINISH_REVOKED;}
static const pet_program_host_backend_t backend={NULL,busy,owner,acquire,current,revoke,finish};
static pet_component_program_config_t config;
static void drain(int32_t *base,unsigned target,uint64_t now){
  for(unsigned i=0;i<100;i++){
    int32_t out[8];if(pet_component_program_session_step(&session,session.revision,now,base,out))memcpy(base,out,sizeof(out));
    if(session.report_valid&&session.last_report.generation==session.generation&&session.last_report.sequence==target)return;
  }assert(!"bounded fixture event did not terminate");
}
static void exercise(void){
  assert(pet_component_program_session_init(&session,&backend));
  pet_program_snapshot_t seed={.schema=config.image.schema,.cells=config.image.cells};uint64_t revision=1;
  int32_t base[8]={0,2},out[8];pet_program_event_t event;
  assert(pet_component_program_seed_valid(&config,&seed));
  stop_on_admission=true;
  assert(pet_component_program_session_start(&session,&config,&seed,revision,0)==PET_RUNTIME_STOPPED);
  assert(admission_stop_count==1&&!held&&!live&&!session.live&&!session.retiring&&!session.starting);
  assert(pet_component_program_session_snapshot(&session,&seed)&&!seed.revision&&!seed.values[0]);
  stop_on_schedule_admission=true;
  assert(pet_component_program_session_start(&session,&config,&seed,revision,0)==PET_RUNTIME_STOPPED);
  assert(admission_stop_count==2&&!held&&!live&&!session.live&&!session.retiring&&!session.scheduled&&session.group.retired);
  admitted=false;assert(pet_component_program_session_start(&session,&config,&seed,revision,0)==PET_RUNTIME_DENIED);assert(!held&&!live);
  admitted=true;assert(pet_component_program_session_start(&session,&config,&seed,revision,0)==PET_RUNTIME_OK);
#if PET_COMPONENT_PROGRAM_RAM_RESUME
  pet_program_snapshot_t resumed={.revision=999},unchanged=resumed;
  assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));
  assert(!memcmp(&resumed,&unchanged,sizeof(resumed))); /* Live inputs stay owned. */
#endif
  assert(session.scheduled&&session.group.host==&session.host&&session.group.scheduler_owner==&session.scheduler);
  pet_program_schedule_t stale_schedule=session.schedule;
  pet_component_program_session_t copy=session,copy_before=copy,original=session;
  pet_program_snapshot_t copied_seed=seed;int32_t untouched[8]={99};
  assert(!pet_component_program_session_can_enqueue(&copy,revision));
  assert(pet_component_program_session_stop(&copy)==PET_RUNTIME_INVALID);
  assert(pet_component_program_session_start(&copy,&config,&seed,revision+1,0)==PET_RUNTIME_INVALID);
  event=(pet_program_event_t){1,{1}};
  assert(pet_component_program_session_enqueue(&copy,revision,&event)==PET_RUNTIME_INVALID);
  assert(!pet_component_program_session_step(&copy,revision,0,base,untouched)&&untouched[0]==99);
  assert(!pet_component_program_session_snapshot(&copy,&copied_seed)&&!memcmp(&copied_seed,&seed,sizeof(seed)));
  assert(!memcmp(&copy,&copy_before,sizeof(copy))&&!memcmp(&session,&original,sizeof(session))&&held&&live);
  assert(pet_component_program_event(&config,"add",base,&event)&&event.count==1&&event.words[0]==2);
  assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_OK);
  assert(!pet_component_program_session_step(&session,revision,1,base,out)); // begin only, no visible partial model
  assert(base[0]==0);drain(base,1,2);assert(base[0]==2&&base[1]==2&&session.last_report.result==PET_PROGRAM_COMMITTED);
  assert(pet_component_program_event(&config,"subtract",base,&event));
  assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_OK);drain(base,2,3);assert(base[0]==1);
  assert(pet_component_program_session_snapshot(&session,&seed)&&seed.values[0]==1&&seed.outputs[0]==1);
  assert(!pet_component_program_session_step(&session,revision,3,base,base)); // no aliased output
  assert(pet_component_program_session_stop(&session)==PET_RUNTIME_OK&&!held&&!live);
#if PET_COMPONENT_PROGRAM_RAM_RESUME
  assert(pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));
  assert(resumed.values[0]==1&&resumed.outputs[0]==1&&resumed.revision==seed.revision);
  unchanged=resumed;
  assert(!pet_component_program_session_resume_snapshot(&session,&config,revision+1,&resumed));
  copy=session;assert(!pet_component_program_session_resume_snapshot(&copy,&config,revision,&resumed));
  pet_component_program_config_t other=config;other.image.code[0].imm^=1;
  assert(!pet_component_program_session_resume_snapshot(&session,&other,revision,&resumed));
  other=config;other.projection.target[0]=PET_PROGRAM_UI_UNBOUND;
  assert(!pet_component_program_session_resume_snapshot(&session,&other,revision,&resumed));
  session.retiring=true;assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));session.retiring=false;
  session.host.inside=true;assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));session.host.inside=false;
  session.host.bound=true;assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));session.host.bound=false;
  session.scheduled=true;assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));session.scheduled=false;
  session.group.retired=false;assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,&resumed));session.group.retired=true;
  assert(!pet_component_program_session_resume_snapshot(&session,&config,revision,(pet_program_snapshot_t*)&session));
  assert(!memcmp(&resumed,&unchanged,sizeof(resumed))&&!held&&!live&&!session.live&&!session.scheduled);
#endif
  assert(!pet_component_program_session_step(&session,revision,100000,base,out));
  assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_STALE);
  // New activation keeps exact state, retires old inputs and creates a fresh schedule.
  config.timer_count=1;config.timers[0]=(pet_program_timer_spec_t){1,200,400,{1,{1}}};
  assert(pet_component_program_session_start(&session,&config,&seed,++revision,500)==PET_RUNTIME_OK);
  assert(session.scheduled&&session.schedule.serial>stale_schedule.serial);
  assert(pet_program_scheduler_remove(&session.scheduler,stale_schedule)==PET_SCHED_STALE&&held&&session.live);
  assert(pet_component_program_session_enqueue(&session,revision-1,&event)==PET_RUNTIME_STALE);
  assert(!pet_component_program_session_step(&session,revision,699,base,out));assert(session.sequence==0);
  drain(base,1,700);assert(base[0]==2&&session.sequence==1);
  assert(!pet_component_program_session_step(&session,revision,699,base,out));assert(!session.live&&!held);
  assert(!pet_component_program_session_step(&session,revision,999999,base,out)); // no clock-recovery restart
  assert(pet_component_program_session_snapshot(&session,&seed)&&seed.values[0]==2);
  assert(session.scheduler.stopped&&!session.scheduled&&session.group.retired);
  config.timer_count=0;
  assert(pet_component_program_session_start(&session,&config,&seed,++revision,1000)==PET_RUNTIME_OK);
  assert(!session.scheduler.stopped&&session.schedule.serial>stale_schedule.serial);
  event=(pet_program_event_t){1,{200}};
  assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_OK);
  drain(base,1,1000);assert(base[0]==2&&!session.live&&!held&&session.last_report.result==PET_PROGRAM_BOUNDS);
  assert(pet_component_program_session_snapshot(&session,&seed)&&seed.values[0]==2&&seed.outputs[0]==2);
  assert(pet_component_program_session_start(&session,&config,&seed,++revision,2000)==PET_RUNTIME_OK);
  event=(pet_program_event_t){1,{3}};assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_OK);
  assert(!pet_component_program_session_step(&session,revision,2000,base,out));audio=true;
  assert(!pet_component_program_session_step(&session,revision,2001,base,out)&&!held&&!session.live);audio=false;
  assert(!pet_component_program_session_step(&session,revision,2002,base,out));
  assert(pet_component_program_session_snapshot(&session,&seed)&&seed.values[0]==2); // no cancelled partial output
  assert(pet_component_program_session_start(&session,&config,&seed,++revision,3000)==PET_RUNTIME_OK);
  session.host.inside=true; // fault injection: physical work has not returned
  assert(pet_component_program_session_stop(&session)==PET_RUNTIME_BUSY&&session.retiring&&held&&!session.live);
  assert(pet_component_program_session_start(&session,&config,&seed,revision+1,3000)==PET_RUNTIME_INVALID&&held);
  session.host.inside=false;
  assert(!pet_component_program_session_step(&session,revision,3001,base,out)&&!held&&!session.retiring);
  assert(pet_component_program_session_start(&session,&config,&seed,++revision,4000)==PET_RUNTIME_OK);
  event=(pet_program_event_t){1,{1}};assert(pet_component_program_session_can_enqueue(&session,revision));
  assert(!pet_component_program_session_can_enqueue(&session,revision-1));
  for(unsigned i=0;i<4;i++)assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_OK);
  assert(!pet_component_program_session_can_enqueue(&session,revision));
  assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_QUEUE_FULL&&session.sequence==4);
  assert(!pet_component_program_session_step(&session,revision,4000,base,out));
  assert(pet_component_program_session_can_enqueue(&session,revision));
  assert(pet_component_program_session_enqueue(&session,revision,&event)==PET_RUNTIME_OK&&session.sequence==5);
  for(unsigned i=1;i<=5;i++)drain(base,i,4000);
  assert(base[0]==7&&session.last_report.revision==seed.revision+5);
  assert(pet_component_program_session_stop(&session)==PET_RUNTIME_OK);
  assert(!pet_component_program_session_can_enqueue(&session,revision));
}
#endif
int main(int argc,char **argv){
  (void)argv;
  if(argc!=2)return 2;char input[65536];size_t length=fread(input,1,sizeof(input)-1,stdin);input[length]=0;
  cJSON *root=cJSON_Parse(input);if(!root)return 3;
  pet_component_program_config_t *out=calloc(1,sizeof(*out));if(!out)return 4;
  bool ok=pet_component_program_parse(root,out);
#if PET_COMPONENT_PROGRAM_ENABLED
  if(!strcmp(argv[1],"session")||!strcmp(argv[1],"resume")){assert(ok);config=*out;exercise();}
#endif
  printf("{\"ok\":%s,\"enabled\":%s}\n",ok?"true":"false",out->enabled?"true":"false");
  free(out);cJSON_Delete(root);return 0;
}
