"""Two real sessions on one real scheduler. Admission callbacks are synthetic
TEST ONLY, not a change to the production zero PROGRAM quota or P4 evidence.
"""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_shared_parent_fairness_exact_reports_aliases_and_retirement(tmp_path):
    code=r'''
#include "pet_p4_component_program_session.h"
#include <assert.h>
#include <string.h>
static pet_resource_broker_t identity;
typedef struct{pet_resource_owner_t owner;pet_resource_lease_t lease;unsigned opens;bool live,held,audio;} mock_t;
static mock_t models[3];static uint64_t generation,serial;
static bool busy(void*ctx){return ((mock_t*)ctx)->audio;}
static pet_resource_result_t open_owner(void*ctx,uint32_t mask,pet_resource_owner_t*out){
 mock_t*m=ctx;assert(mask==PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM)&&!m->live&&!m->held);m->opens++;
 m->owner=(pet_resource_owner_t){&identity,++generation,0};m->live=true;*out=m->owner;return PET_RESOURCE_OK;}
static pet_resource_result_t acquire(void*ctx,pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t*out){
 mock_t*m=ctx;assert(m->live&&!m->held&&owner.generation==m->owner.generation&&kind==PET_RESOURCE_PROGRAM&&units==64);
 m->lease=(pet_resource_lease_t){owner,++serial,units,kind,0};m->held=true;*out=m->lease;return PET_RESOURCE_OK;}
static bool current(void*ctx,pet_resource_lease_t lease){mock_t*m=ctx;return m->live&&m->held&&lease.serial==m->lease.serial&&lease.owner.generation==m->owner.generation;}
static void revoke(void*ctx,pet_resource_owner_t owner){mock_t*m=ctx;assert(owner.generation==m->owner.generation);m->live=false;}
static pet_resource_finish_t finish(void*ctx,pet_resource_lease_t lease){mock_t*m=ctx;assert(m->held&&lease.serial==m->lease.serial);m->held=false;return PET_RESOURCE_FINISH_REVOKED;}
static pet_component_program_session_t sessions[2],fresh;
static pet_program_scheduler_t shared;
static const pet_program_insn_t instructions[]={
 {PET_OP_CONST,0,0,0,0},{PET_OP_LOAD,1,0,0,0},{PET_OP_EVENT,2,0,0,0},
 {PET_OP_ADD,1,1,2,0},{PET_OP_STORE,0,1,0,0},{PET_OP_OUTPUT,0,1,0,0},{PET_OP_HALT,0,0,0,0}};
int main(void){
 pet_component_program_config_t config={.enabled=true};
 config.image=(pet_program_image_t){.schema=1,.count=7,.fuel=64,.cells=1,.valid=true};memcpy(config.image.code,instructions,sizeof(instructions));
 config.projection=(pet_program_ui_projection_t){.schema=1,.variables=1,.minimum={0},.maximum={100}};
 memset(config.projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(config.projection.target));config.projection.target[0]=0;
 assert(pet_program_scheduler_init(&shared));pet_program_snapshot_t seed={.schema=1,.cells=1};
 for(unsigned i=0;i<2;i++){
  pet_program_host_backend_t backend={&models[i],busy,open_owner,acquire,current,revoke,finish};
  assert(pet_component_program_session_init(&sessions[i],&backend));
  assert(!pet_component_program_session_bind_scheduler(&sessions[i],&sessions[i].scheduler));
  assert(pet_component_program_session_bind_scheduler(&sessions[i],&shared));
  assert(!pet_component_program_session_bind_scheduler(&sessions[i],&shared));
  assert(pet_component_program_session_start(&sessions[i],&config,&seed,1,0)==PET_RUNTIME_OK);
  assert(!sessions[i].scheduler.instance&&sessions[i].schedule.scheduler==&shared);
 }
 int32_t base[2][8]={{0}},out[8]={123};unsigned results[2]={0},selected[2]={0};
 for(unsigned i=0;i<2;i++){pet_program_event_t event={1,{i?10:1}};assert(pet_component_program_session_enqueue(&sessions[i],1,&event)==PET_RUNTIME_OK);}
 uint64_t before_serial=shared.next_serial;
 assert(!pet_component_program_session_step(&sessions[0],1,0,base[0],out)&&out[0]==123&&shared.next_serial==before_serial);
 bool alias_checked=false;
 for(unsigned turn=0;turn<20;turn++){
  pet_program_schedule_diagnostic_t d={0};assert(pet_program_scheduler_step(&shared,turn,&d)==PET_SCHED_TURN);
  assert(d.schedule.slot==turn%2);selected[d.schedule.slot]++;
  for(unsigned i=0;i<2;i++){
   if(sessions[i].host.runtime.report_pending){
    shared.inside=true;
    assert(!pet_component_program_session_collect(&sessions[i],1,base[i],out)&&sessions[i].host.runtime.report_pending);
    shared.inside=false;
    assert(!pet_component_program_session_collect(&sessions[i],1,base[i],(int32_t*)&shared.slots[0]));
    assert(!pet_component_program_session_collect(&sessions[i],1,base[i],sessions[1-i].host.runtime.state.outputs));
    assert(sessions[i].host.runtime.report_pending);alias_checked=true;
   }
   if(pet_component_program_session_collect(&sessions[i],1,base[i],out)){memcpy(base[i],out,sizeof(out));results[i]++;}
  }
  if(results[0]&&results[1])break;
 }
 assert(alias_checked&&results[0]==1&&results[1]==1&&base[0][0]==1&&base[1][0]==10&&selected[0]==selected[1]);
 for(unsigned i=0;i<2;i++){out[0]=333;assert(!pet_component_program_session_collect(&sessions[i],1,base[i],out)&&out[0]==333);}
 pet_component_program_session_t copied=sessions[0];assert(!pet_component_program_session_collect(&copied,1,base[0],out));
 assert(!pet_component_program_session_bind_scheduler(&sessions[0],&shared));
 pet_program_host_backend_t backend={&models[2],busy,open_owner,acquire,current,revoke,finish};
 assert(pet_component_program_session_init(&fresh,&backend));pet_program_scheduler_t moved=shared;
 assert(!pet_component_program_session_bind_scheduler(&fresh,&moved));
 assert(pet_component_program_session_bind_scheduler(&fresh,&shared));
 shared.inside=true;unsigned fresh_opens=models[2].opens;
 assert(pet_component_program_session_start(&fresh,&config,&seed,1,50)==PET_RUNTIME_BUSY&&models[2].opens==fresh_opens);
 pet_program_event_t reentry={1,{1}};uint64_t old_sequence=sessions[0].sequence;
 assert(!pet_component_program_session_can_enqueue(&sessions[0],1));
 assert(pet_component_program_session_enqueue(&sessions[0],1,&reentry)==PET_RUNTIME_BUSY&&sessions[0].sequence==old_sequence);
 shared.inside=false;
 assert(!pet_program_scheduler_disjoint(&shared,NULL,1)&&!pet_program_scheduler_disjoint(&shared,out,0));
 pet_program_schedule_t old=sessions[0].schedule;
 assert(pet_component_program_session_snapshot(&sessions[0],&seed));
 assert(pet_component_program_session_stop(&sessions[0])==PET_RUNTIME_OK&&!models[0].held&&models[1].held);
 assert(pet_component_program_session_can_enqueue(&sessions[1],1));
 assert(pet_component_program_session_start(&sessions[0],&config,&seed,2,50)==PET_RUNTIME_OK);
 assert(sessions[0].schedule.serial>old.serial&&pet_program_scheduler_remove(&shared,old)==PET_SCHED_STALE&&sessions[0].live);
 assert(pet_program_scheduler_stop(&shared)==PET_SCHED_STOPPED);
 for(unsigned i=0;i<2;i++){
  assert(!pet_component_program_session_collect(&sessions[i],sessions[i].revision,base[i],out));
  assert(!sessions[i].live&&!sessions[i].scheduled&&!models[i].held&&!models[i].live);
 }
 unsigned opens=models[0].opens;
 assert(pet_component_program_session_start(&sessions[0],&config,&seed,3,60)==PET_RUNTIME_STOPPED&&models[0].opens==opens);
 assert(pet_program_scheduler_rearm(&shared)==PET_SCHED_OK);
 assert(pet_component_program_session_start(&sessions[0],&config,&seed,3,60)==PET_RUNTIME_OK);
 assert(pet_component_program_session_stop(&sessions[0])==PET_RUNTIME_OK);
 assert(!models[0].held&&!models[1].held);return 0;
}
'''
    path=tmp_path/'shared.c';path.write_text(code);binary=tmp_path/'shared'
    names=('component_program_session','program_core','program_image','program_runtime','program_host','program_group','program_scheduler','program_timers','event_bus')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DPET_COMPONENT_PROGRAM_ENABLED=1',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(path),
        *[str(ROOT/'main'/f'pet_p4_{name}.c') for name in names],'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
