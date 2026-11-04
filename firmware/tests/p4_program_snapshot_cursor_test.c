/* Real runtime commits/publication; cursor is never a guest/wire version. */
#include "pet_p4_program_runtime.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static pet_program_runtime_t r,other;
static pet_program_image_t image;
static unsigned authority_calls;
static bool allowed=true;
static uint64_t generation=1;
static bool current(void *ctx,uint64_t g){(void)ctx;authority_calls++;return allowed&&g==generation;}
static bool publish(void *ctx,uint64_t g,pet_program_state_t *target,const pet_program_state_t *candidate){
 (void)ctx;assert(g==generation);
#if PET_P4_PROGRAM_V2
 pet_program_publication_t *p=target->publication;
 pet_program_state_t *spare=p->live==p->slots[0]?p->slots[1]:p->slots[0];
 *spare=*candidate;p->live=spare;
#else
 *target=*candidate;
#endif
 return true;
}
static void activate(pet_program_runtime_t *runtime,uint64_t gen,const pet_program_snapshot_t *seed){
 assert(pet_program_runtime_start_with_publication(runtime,&image,gen,current,NULL,1,seed,NULL,publish,NULL)==PET_RUNTIME_OK);
}
static pet_program_report_t commit(int value,uint64_t seq){
 pet_program_event_t event={1,{value}};assert(pet_program_runtime_enqueue(&r,generation,seq,&event)==PET_RUNTIME_OK);
 pet_program_runtime_result_t result=PET_RUNTIME_INVALID;
 for(unsigned i=0;i<12;i++){result=pet_program_runtime_pump(&r,generation);if(result==PET_RUNTIME_COMPLETE)break;assert(result==PET_RUNTIME_YIELDED);}
 assert(result==PET_RUNTIME_COMPLETE);pet_program_report_t report;
 assert(pet_program_runtime_take_report(&r,generation,&report)==PET_RUNTIME_OK);return report;
}
int main(void){
 image=(pet_program_image_t){.schema=91,.count=5,.fuel=64,.cells=PET_PROGRAM_CELLS,.valid=true};
 image.code[0]=(pet_program_insn_t){PET_OP_CONST,0,0,0,PET_PROGRAM_CELLS-1};
 image.code[1]=(pet_program_insn_t){PET_OP_EVENT,1,0,0,0};
 image.code[2]=(pet_program_insn_t){PET_OP_STORE,0,1,0,0};
 image.code[3]=(pet_program_insn_t){PET_OP_OUTPUT,0,1,0,0};
 image.code[4]=(pet_program_insn_t){PET_OP_HALT,0,0,0,0};
 pet_program_runtime_init(&r);activate(&r,1,NULL);
 pet_program_snapshot_t snapshot,sentinel,expected;pet_program_snapshot_cursor_t cursor,next;
 memset(&snapshot,0xa5,sizeof snapshot);
 assert(pet_program_runtime_snapshot_if_changed(&r,1,NULL,&snapshot,&cursor)==PET_RUNTIME_OK);
 memset(&expected,0,sizeof expected);expected.schema=91;expected.cells=PET_PROGRAM_CELLS;
 assert(!memcmp(&snapshot,&expected,sizeof snapshot)); // Canonical header/padding/tail.
 assert(cursor.runtime==&r&&cursor.generation==1&&snapshot.revision==0&&snapshot.values[PET_PROGRAM_CELLS-1]==0);
 memset(&snapshot,0xa5,sizeof snapshot);sentinel=snapshot;unsigned calls=authority_calls;
 assert(pet_program_runtime_snapshot_if_changed(&r,1,&cursor,&snapshot,&next)==PET_RUNTIME_UNCHANGED);
 assert(!memcmp(&snapshot,&sentinel,sizeof snapshot)&&authority_calls==calls);
 /* Private work modifies the candidate, not the committed version or cache. */
 pet_program_event_t e={1,{41}};assert(pet_program_runtime_enqueue(&r,1,1,&e)==PET_RUNTIME_OK);
 for(unsigned i=0;i<6;i++){
  assert(pet_program_runtime_pump(&r,1)==PET_RUNTIME_YIELDED);
  assert(pet_program_runtime_snapshot_if_changed(&r,1,&cursor,&snapshot,&next)==PET_RUNTIME_UNCHANGED);
  assert(!memcmp(&snapshot,&sentinel,sizeof snapshot));
 }
 assert(r.workspace.result==PET_PROGRAM_READY);
 assert(pet_program_runtime_pump(&r,1)==PET_RUNTIME_COMPLETE);
 assert(pet_program_runtime_snapshot_if_changed(&r,1,&cursor,&snapshot,&next)==PET_RUNTIME_OK);
 assert(snapshot.revision==1&&snapshot.values[PET_PROGRAM_CELLS-1]==41&&snapshot.outputs[0]==41);cursor=next;
 expected.revision=1;expected.values[PET_PROGRAM_CELLS-1]=41;expected.outputs[0]=41;
 assert(!memcmp(&snapshot,&expected,sizeof snapshot));
 /* Historical observation retains final commit/report even after revocation. */
 allowed=false;calls=authority_calls;
 memset(&snapshot,0xa5,sizeof snapshot);
 assert(pet_program_runtime_snapshot_if_changed(&r,1,&cursor,&snapshot,&next)==PET_RUNTIME_UNCHANGED&&authority_calls==calls);
 pet_program_report_t report;assert(pet_program_runtime_take_report(&r,1,&report)==PET_RUNTIME_OK&&report.result==PET_PROGRAM_COMMITTED);
 assert(pet_program_runtime_cancel(&r,1)==PET_RUNTIME_OK);allowed=true;
 assert(pet_program_runtime_snapshot_if_changed(&r,1,&cursor,&snapshot,&next)==PET_RUNTIME_UNCHANGED);
 /* Same object, schema, cells, revision but a NEW generation with changed data. */
 pet_program_snapshot_t seed={.schema=91,.revision=1,.cells=PET_PROGRAM_CELLS,.outputs={88}};seed.values[PET_PROGRAM_CELLS-1]=88;
 generation=2;activate(&r,2,&seed);
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,&next)==PET_RUNTIME_OK);
 assert(snapshot.values[PET_PROGRAM_CELLS-1]==88&&snapshot.outputs[0]==88&&next.generation==2);cursor=next;
 /* Same generation/revision/layout on a distinct owner cannot reuse a cursor. */
 pet_program_runtime_init(&other);seed.outputs[0]=19;seed.values[PET_PROGRAM_CELLS-1]=19;activate(&other,2,&seed);
 assert(pet_program_runtime_snapshot_if_changed(&other,2,&cursor,&snapshot,&next)==PET_RUNTIME_OK&&snapshot.outputs[0]==19&&next.runtime==&other);
 /* Bad seed does not change a valid current lifetime; stale/corrupt cursors miss. */
 pet_program_snapshot_cursor_t bad=cursor;bad.schema++;
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&bad,&snapshot,&next)==PET_RUNTIME_OK&&snapshot.outputs[0]==88);
 bad=cursor;bad.cells=0;assert(pet_program_runtime_snapshot_if_changed(&r,2,&bad,&snapshot,&next)==PET_RUNTIME_OK);
 bad=cursor;bad.revision++;assert(pet_program_runtime_snapshot_if_changed(&r,2,&bad,&snapshot,&next)==PET_RUNTIME_OK);
#if PET_P4_PROGRAM_V2
 /* Bound facade arrays are intentionally stale; read actual selected slot. */
 r.state.cells[PET_PROGRAM_CELLS-1]=-77;r.state.outputs[0]=-77;
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,&next)==PET_RUNTIME_UNCHANGED);
 pet_program_publication_t *p=r.state.publication;const void *self=p->self;p->self=NULL;
 memset(&snapshot,0xa5,sizeof snapshot);sentinel=snapshot;memset(&next,0x5a,sizeof next);bad=next;
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,&next)==PET_RUNTIME_BAD_STATE);
 assert(!memcmp(&snapshot,&sentinel,sizeof snapshot)&&!memcmp(&next,&bad,sizeof next));p->self=self;
#endif
 assert(commit(99,1).result==PET_PROGRAM_COMMITTED);
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,&next)==PET_RUNTIME_OK&&snapshot.outputs[0]==99&&snapshot.values[PET_PROGRAM_CELLS-1]==99);cursor=next;
 /* Every invalid/busy/stale observation leaves both output buffers untouched. */
 memset(&snapshot,0xa5,sizeof snapshot);sentinel=snapshot;memset(&next,0x5a,sizeof next);bad=next;
 assert(pet_program_runtime_snapshot_if_changed(&r,1,&cursor,&snapshot,&next)==PET_RUNTIME_STALE);
 assert(!memcmp(&snapshot,&sentinel,sizeof snapshot)&&!memcmp(&next,&bad,sizeof next));
 r.inside=true;assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,&next)==PET_RUNTIME_BUSY);r.inside=false;
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,&cursor)==PET_RUNTIME_INVALID);
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,(pet_program_snapshot_t*)&r.state,&next)==PET_RUNTIME_INVALID);
 assert(pet_program_runtime_snapshot_if_changed(&r,2,(const pet_program_snapshot_cursor_t*)&r.image,&snapshot,&next)==PET_RUNTIME_INVALID);
 assert(pet_program_runtime_snapshot_if_changed(&r,2,&cursor,&snapshot,(pet_program_snapshot_cursor_t*)&snapshot)==PET_RUNTIME_INVALID);
 pet_program_runtime_t copied=r;assert(pet_program_runtime_snapshot_if_changed(&copied,2,&cursor,&snapshot,&next)==PET_RUNTIME_INVALID);
 assert(!memcmp(&snapshot,&sentinel,sizeof snapshot)&&!memcmp(&next,&bad,sizeof next));
 /* Layout/schema change and overflow: no reuse, wrap, or reset of revision. */
 assert(pet_program_runtime_cancel(&r,2)==PET_RUNTIME_OK);generation=3;image.schema=92;image.cells=2;image.code[0].imm=1;
 seed=(pet_program_snapshot_t){.schema=92,.cells=2,.revision=UINT32_MAX,.outputs={7}};seed.values[1]=7;activate(&r,3,&seed);
 assert(pet_program_runtime_snapshot_if_changed(&r,3,&cursor,&snapshot,&next)==PET_RUNTIME_OK&&snapshot.schema==92&&snapshot.cells==2&&snapshot.revision==UINT32_MAX);cursor=next;
 assert(commit(77,1).result==PET_PROGRAM_OVERFLOW);
 assert(pet_program_runtime_snapshot_if_changed(&r,3,&cursor,&snapshot,&next)==PET_RUNTIME_UNCHANGED);
 assert(snapshot.values[1]==7&&snapshot.outputs[0]==7&&snapshot.revision==UINT32_MAX);
 /* Fault injection into trusted state: full capture must retain nonzero
  * unused cells for seed_valid to reject, never normalize/hide corruption. */
 pet_program_state_t *live=(pet_program_state_t*)pet_program_state_read(&r.state);
 live->cells[PET_PROGRAM_CELLS-1]=55;
 assert(pet_program_runtime_snapshot_if_changed(&r,3,NULL,&snapshot,&next)==PET_RUNTIME_OK);
 memset(&expected,0,sizeof expected);expected.schema=92;expected.cells=2;expected.revision=UINT32_MAX;
 expected.values[1]=7;expected.values[PET_PROGRAM_CELLS-1]=55;expected.outputs[0]=7;
 assert(!memcmp(&snapshot,&expected,sizeof snapshot));
 memset(&snapshot,0xa5,sizeof snapshot);assert(pet_program_runtime_snapshot(&r,3,&snapshot)==PET_RUNTIME_OK);
 assert(!memcmp(&snapshot,&expected,sizeof snapshot));
 puts("PASS cursor: unchanged bytes untouched, real changed commits/generation/layout, publication, revoke, aliases, overflow");
 return 0;
}
