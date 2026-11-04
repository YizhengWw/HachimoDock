"""Future-capacity fault model only; production PROGRAM/PERIPHERAL stay zero.

Compile a private temporary broker copy with capacity to prove real realtime
admission/revocation semantics independently of delayed audio flags. No device,
drivers, audio, installer or production quota changes.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_program_and_native_provider_share_atomic_realtime_exclusion(tmp_path):
    path = ROOT / 'main/pet_p4_resources.c'
    original = path.read_text()
    assert '[PET_RESOURCE_PROGRAM]={0,0}' in original
    assert '[PET_RESOURCE_PERIPHERAL]={0,0}' in original
    source = tmp_path / 'broker.c'
    source.write_text(original.replace('[PET_RESOURCE_PROGRAM]={0,0}', '[PET_RESOURCE_PROGRAM]={1,4096}')
                     .replace('[PET_RESOURCE_PERIPHERAL]={0,0}', '[PET_RESOURCE_PERIPHERAL]={1,16}'))
    test = tmp_path / 'future.c'
    test.write_text(r'''
#include "pet_p4_resources.h"
#include <assert.h>
int main(void){
 pet_resource_broker_t broker={0};pet_resource_owner_t owner={0},next={0};
 pet_resource_lease_t program={0},provider={0},pending={0};pet_resource_realtime_t audio={0};
 uint32_t allowed=PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM)|PET_RESOURCE_MASK(PET_RESOURCE_PERIPHERAL);
 assert(pet_resources_owner_open(&broker,allowed,&owner)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,owner,PET_RESOURCE_PROGRAM,64,&program)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,owner,PET_RESOURCE_PERIPHERAL,1,&provider)==PET_RESOURCE_OK);
 assert(pet_resources_realtime_begin(&broker,&audio)==PET_RESOURCE_OK);
 assert(!pet_resources_current(&broker,program)&&!pet_resources_current(&broker,provider));
 pet_resource_usage_t used;pet_resources_usage(&broker,&used);
 assert(used.units[PET_RESOURCE_PROGRAM]==64&&used.units[PET_RESOURCE_PERIPHERAL]==1);
 assert(used.protected_mask&PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM));
 assert(used.protected_mask&PET_RESOURCE_MASK(PET_RESOURCE_PERIPHERAL));
 assert(pet_resources_owner_open(&broker,allowed,&next)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,next,PET_RESOURCE_PROGRAM,64,&pending)==PET_RESOURCE_PROTECTED);
 assert(pet_resources_try_acquire(&broker,next,PET_RESOURCE_PERIPHERAL,1,&pending)==PET_RESOURCE_PROTECTED);
 assert(!pending.serial);
 assert(pet_resources_realtime_finish(&broker,audio));
 assert(!pet_resources_owner_current(&broker,owner));
 // Physical occupancy outlives cancellation, and prevents reuse until finish.
 assert(pet_resources_try_acquire(&broker,next,PET_RESOURCE_PROGRAM,64,&pending)==PET_RESOURCE_BUSY);
 assert(pet_resources_finish(&broker,program)==PET_RESOURCE_FINISH_REVOKED);
 assert(pet_resources_finish(&broker,provider)==PET_RESOURCE_FINISH_REVOKED);
 assert(pet_resources_try_acquire(&broker,next,PET_RESOURCE_PROGRAM,64,&pending)==PET_RESOURCE_OK);
 assert(pet_resources_owner_revoke(&broker,next));
 assert(pet_resources_finish(&broker,pending)==PET_RESOURCE_FINISH_REVOKED);
 pet_resources_usage(&broker,&used);assert(!used.live_owners&&!used.revoked_owners);
 return 0;
}
''')
    executable = tmp_path / 'future'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
                    str(source), str(test), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    assert path.read_text() == original
