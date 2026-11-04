/* Factory only. No global program, scheduling, installation or wire routing.
 * The immutable backend uses the same device resource broker and four read-only
 * audio flags. Production quota is zero; creating a host grants no execution.
 * See program_host.h: the future caller must provide lifecycle/state exclusion,
 * including the commit/audio transition boundary; this factory is not a mutex.
 */
#pragma once
#include "pet_p4_program_host.h"
bool pet_program_host_device_init(pet_program_host_t *fresh_host);
/* Fixed native diagnostic vectors only. Uses real NATIVE_PROBE admission and
 * atomic publication, not a fake PROGRAM ticket or an installable-code route. */
bool pet_program_host_device_init_native_probe(pet_program_host_t *fresh_host);
