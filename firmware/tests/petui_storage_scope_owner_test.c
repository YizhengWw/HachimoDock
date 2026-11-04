#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_storage_lane.h"

static uintptr_t task=7;
static unsigned held=1,unlocks,locks;
static uintptr_t owner(void *context){(void)context;return task;}
static void unlock(void *context){(void)context;assert(held);held=0;unlocks++;}
static void lock(void *context){(void)context;assert(!held);held=1;locks++;}
static void start(pet_storage_lane_t *lane,pet_storage_scope_t *scope){
  assert(pet_storage_lane_configure(lane,unlock,lock,owner,NULL));
  assert(pet_storage_lane_enter(lane));
  assert(pet_storage_scope_begin(lane,scope));
}
static void finish(pet_storage_lane_t *lane,pet_storage_scope_t *scope){
  assert(pet_storage_scope_finish(scope));assert(held&&!lane->occupied);
  assert(pet_storage_lane_leave(lane));assert(locks==unlocks);
}
static void copy_locked(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};start(&lane,&live);
  pet_storage_scope_t copy=live,before=live;pet_storage_lane_t before_lane=lane;
  assert(!pet_storage_scope_unlock(&copy));assert(held&&!unlocks);
  assert(!pet_storage_scope_lock(&copy));
  assert(!pet_storage_scope_cancel_allowed(&copy));
  assert(!pet_storage_scope_advance(&copy,PET_STORAGE_PREPARE));
  assert(!pet_storage_scope_finish(&copy));
  assert(!memcmp(&before,&live,sizeof(live))&&!memcmp(&before_lane,&lane,sizeof(lane)));
  finish(&lane,&live);
}
static void copy_unlocked(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};start(&lane,&live);
  assert(pet_storage_scope_advance(&live,PET_STORAGE_PREPARE));
  assert(pet_storage_scope_unlock(&live)&&!held);
  pet_storage_scope_t copy=live,before=live;pet_storage_lane_t before_lane=lane;
  assert(!pet_storage_scope_lock(&copy)&&!held&&!locks);
  assert(!pet_storage_scope_finish(&copy)&&!held);
  assert(!pet_storage_scope_advance(&copy,PET_STORAGE_DURABLE));
  assert(!memcmp(&before,&live,sizeof(live))&&!memcmp(&before_lane,&lane,sizeof(lane)));
  finish(&lane,&live);
}
static void no_rebind(void){
  pet_storage_lane_t a={0},b={0};pet_storage_scope_t live={0},other={0};start(&a,&live);
  assert(pet_storage_lane_configure(&b,unlock,lock,owner,NULL));
  assert(pet_storage_lane_enter(&b));
  pet_storage_scope_t before=live;pet_storage_lane_t before_a=a,before_b=b;
  assert(!pet_storage_scope_begin(&b,&live));
  assert(!memcmp(&before,&live,sizeof(live))&&!memcmp(&before_a,&a,sizeof(a))&&!memcmp(&before_b,&b,sizeof(b)));
  assert(pet_storage_scope_begin(&b,&other));
  assert(pet_storage_scope_finish(&other));assert(pet_storage_lane_leave(&b));
  finish(&a,&live);
}
static void no_copied_publish(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};start(&lane,&live);
  assert(pet_storage_scope_advance(&live,PET_STORAGE_PREPARE));
  assert(pet_storage_scope_unlock(&live));
  assert(pet_storage_scope_advance(&live,PET_STORAGE_DURABLE));
  pet_storage_scope_t copy=live;
  assert(!pet_storage_scope_lock(&copy)&&!held);
  assert(pet_storage_scope_lock(&live)&&held);
  assert(!pet_storage_scope_advance(&copy,PET_STORAGE_PUBLISHED));
  assert(!pet_storage_scope_finish(&copy));
  assert(!pet_storage_scope_finish(&live)&&lane.occupied);
  assert(pet_storage_scope_advance(&live,PET_STORAGE_PUBLISHED));
  assert(pet_storage_scope_advance(&live,PET_STORAGE_CLEANUP));
  finish(&lane,&live);
}
static void original_reuse(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};start(&lane,&live);
  for(unsigned i=0;i<10000;i++){
    pet_storage_scope_t copy=live;
    assert(!pet_storage_scope_finish(&copy));
    assert(pet_storage_scope_unlock(&live)&&!held);
    assert(pet_storage_scope_finish(&live)&&held&&!lane.occupied);
    assert(pet_storage_scope_begin(&lane,&live));
    assert(!pet_storage_scope_finish(&copy));
  }
  finish(&lane,&live);
}
static void task_isolation(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};start(&lane,&live);
  assert(pet_storage_scope_unlock(&live));task=8;
  assert(!pet_storage_scope_lock(&live)&&!held);
  assert(!pet_storage_scope_finish(&live)&&lane.occupied);
  assert(!pet_storage_scope_advance(&live,PET_STORAGE_PREPARE));
  task=7;finish(&lane,&live);
}
static void retired_copy(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};start(&lane,&live);
  pet_storage_scope_t copy=live;
  assert(pet_storage_scope_finish(&live));
  assert(pet_storage_scope_begin(&lane,&live));
  assert(!pet_storage_scope_unlock(&copy));assert(!pet_storage_scope_finish(&copy));
  finish(&lane,&live);
}
static void no_lane_alias(void){
  union {pet_storage_lane_t lane;pet_storage_scope_t scope;} same={0};
  unsigned char before[sizeof(same)];memcpy(before,&same,sizeof(same));
  assert(!pet_storage_scope_begin(&same.lane,&same.scope));
  assert(!memcmp(before,&same,sizeof(same)));
}
static void unidentified_task(void){
  pet_storage_lane_t lane={0};pet_storage_scope_t live={0};
  assert(pet_storage_lane_configure(&lane,unlock,lock,owner,NULL));
  task=0;pet_storage_lane_t before_lane=lane;pet_storage_scope_t before=live;
  assert(!pet_storage_scope_begin(&lane,&live));
  assert(!memcmp(&before,&live,sizeof(live))&&!memcmp(&before_lane,&lane,sizeof(lane)));
  assert(held&&!locks&&!unlocks);task=7;
  assert(pet_storage_lane_enter(&lane));assert(pet_storage_scope_begin(&lane,&live));
  finish(&lane,&live);
}
int main(int argc,char **argv){
  assert(argc==2);switch(atoi(argv[1])){
    case 0:copy_locked();break;case 1:copy_unlocked();break;
    case 2:no_rebind();break;case 3:no_copied_publish();break;
    case 4:original_reuse();break;case 5:task_isolation();break;
    case 6:retired_copy();break;case 7:no_lane_alias();break;
    case 8:unidentified_task();break;
    default:assert(0);
  }
  return 0;
}
