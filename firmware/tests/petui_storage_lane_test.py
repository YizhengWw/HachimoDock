"""Production single storage job: explicit authority, ownership and cancellation."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_explicit_owner_scope_preserves_lock_and_durable_publication(tmp_path):
    source = tmp_path / 'lane.c'
    source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "pet_p4_storage_lane.h"
static uintptr_t task=1;
static unsigned held=1,unlocks,locks;
static uintptr_t owner(void *context){(void)context;return task;}
static void unlock(void *context){(void)context;assert(held);held=0;unlocks++;}
static void lock(void *context){(void)context;assert(!held);held=1;locks++;}
int main(void){
 pet_storage_lane_t lane={0};pet_storage_scope_t s={0},other={0};
 // Boot has no registered callbacks and cannot accidentally unlock anything.
 assert(pet_storage_scope_begin(&lane,&s));assert(pet_storage_scope_unlock(&s));
 assert(held&&unlocks==0);assert(pet_storage_scope_finish(&s));
 assert(pet_storage_lane_configure(&lane,unlock,lock,owner,0));
 // Registering callbacks is NOT a grant. A fallback keeps synchronous locking.
 assert(pet_storage_scope_begin(&lane,&s));assert(pet_storage_scope_unlock(&s));
 assert(held&&unlocks==0);assert(pet_storage_scope_finish(&s));
 assert(pet_storage_lane_enter(&lane));assert(!pet_storage_lane_enter(&lane));
 // Runtime native fallback has no command token and must not borrow another
 // task's authority before that command has reserved its own physical job.
 task=2;uint64_t serial=lane.job_serial;
 assert(!pet_storage_scope_begin(&lane,&other)&&!lane.occupied&&lane.job_serial==serial);
 assert(!pet_storage_lane_leave(&lane));task=1;
 assert(pet_storage_scope_begin(&lane,&s));assert(!pet_storage_scope_begin(&lane,&other));
 assert(!pet_storage_lane_leave(&lane));assert(pet_storage_scope_cancel_allowed(&s));
 assert(pet_storage_scope_advance(&s,PET_STORAGE_PREPARE));
 assert(pet_storage_scope_unlock(&s)&&!held);assert(!pet_storage_scope_unlock(&s));
 task=2;assert(!pet_storage_scope_lock(&s)&&!held);assert(!pet_storage_scope_finish(&s));
 assert(!pet_storage_scope_advance(&s,PET_STORAGE_DURABLE));
 task=1;pet_storage_scope_t stale=s;
 assert(pet_storage_scope_finish(&s)&&held&&locks==1&&unlocks==1);
 // Cleanup of a stale scope cannot release a newer job's physical occupancy.
 assert(pet_storage_scope_begin(&lane,&s));
 assert(!pet_storage_scope_finish(&stale)&&lane.occupied);
 assert(pet_storage_scope_advance(&s,PET_STORAGE_PREPARE));
 assert(pet_storage_scope_unlock(&s));
 assert(pet_storage_scope_advance(&s,PET_STORAGE_DURABLE));
 assert(!pet_storage_scope_cancel_allowed(&s));
 assert(!pet_storage_scope_advance(&s,PET_STORAGE_PUBLISHED));
 assert(!pet_storage_scope_finish(&s)&&held&&lane.occupied);
 assert(pet_storage_scope_advance(&s,PET_STORAGE_PUBLISHED));
 assert(pet_storage_scope_advance(&s,PET_STORAGE_CLEANUP));
 assert(pet_storage_scope_unlock(&s));assert(!pet_storage_scope_begin(&lane,&other));
 assert(pet_storage_scope_finish(&s)&&held&&!lane.occupied);
 assert(pet_storage_lane_leave(&lane));assert(!pet_storage_lane_leave(&lane));
 lane.job_serial=UINT64_MAX;assert(!pet_storage_scope_begin(&lane,&s));
 lane.command_serial=UINT64_MAX;assert(!pet_storage_lane_enter(&lane));
 assert(locks==unlocks);return 0;
}
''')
    exe = tmp_path / 'lane'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
                    str(source), str(ROOT / 'main/pet_p4_storage_lane.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
