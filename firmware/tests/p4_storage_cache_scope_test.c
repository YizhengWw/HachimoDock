/* Actual storage lane + cache parent hooks, counted observation only. No IO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_storage_lane.h"
#include "pet_p4_storage_io_trace.h"
static pet_storage_lane_t g_storage_lane;
static unsigned begins,stops,locks,unlocks;
static uint32_t generation=7,stopped;
static bool available=true;
static uintptr_t task=1;
uint32_t pet_storage_io_begin(uint32_t duration){assert(duration==5000000);begins++;return available?generation++:0;}
bool pet_storage_io_stop(uint32_t g){stops++;stopped=g;return true;}
static void unlock(void *p){assert(p==(void*)1);unlocks++;}
static void lock(void *p){assert(p==(void*)1);locks++;}
static uintptr_t owner(void *p){assert(p==(void*)1);return task;}
#include "pet_p4_storage_cache_scope.inc"
static void enter(pet_storage_scope_t *s){assert(pet_storage_lane_enter(&g_storage_lane));assert(pet_storage_scope_begin(&g_storage_lane,s));}
static void finish(pet_storage_scope_t *s){assert(pet_storage_scope_finish(s));storage_cache_scope_finish(s);assert(pet_storage_lane_leave(&g_storage_lane));}
int main(void){
 assert(pet_storage_lane_configure(&g_storage_lane,unlock,lock,owner,(void*)1));
 pet_storage_scope_t s={0};enter(&s);storage_cache_scope_begin(&s);
 assert(begins==1&&g_storage_cache_scope.scope==&s&&g_storage_cache_scope.generation==7);
 storage_cache_scope_begin(&s);assert(begins==1);
 pet_storage_scope_t copy=s;storage_cache_scope_begin(&copy);assert(begins==1);
 assert(!pet_storage_scope_finish(&copy));storage_cache_scope_finish(&copy);assert(!stops);
 assert(pet_storage_scope_unlock(&s));assert(unlocks==1);
 storage_cache_scope_finish(&s);assert(!stops);finish(&s);assert(locks==1&&stops==1&&stopped==7&&!g_storage_cache_scope.scope);
 storage_cache_scope_finish(&s);storage_cache_scope_finish(&copy);assert(stops==1);
 // A separately armed diagnostic window returns busy: never adopt or stop it.
 available=false;enter(&s);storage_cache_scope_begin(&s);assert(begins==2&&!g_storage_cache_scope.scope);finish(&s);assert(stops==1);
 available=true;enter(&s);storage_cache_scope_begin(&s);assert(begins==3&&g_storage_cache_scope.generation==8);
 task=2;assert(!pet_storage_scope_finish(&s));storage_cache_scope_finish(&s);assert(stops==1);task=1;
 // An unpublished durable result cannot release either the real lane or cache.
 assert(pet_storage_scope_advance(&s,PET_STORAGE_PREPARE));assert(pet_storage_scope_advance(&s,PET_STORAGE_DURABLE));
 assert(!pet_storage_scope_finish(&s));storage_cache_scope_finish(&s);assert(stops==1);
 assert(pet_storage_scope_advance(&s,PET_STORAGE_PUBLISHED));finish(&s);assert(stops==2&&stopped==8);
 storage_cache_scope_finish(&copy);assert(stops==2&&!g_storage_cache_scope.scope);
 puts("PASS cache lifetime follows exact native scope completion; busy and copied owners cannot stop another window");return 0;
}
