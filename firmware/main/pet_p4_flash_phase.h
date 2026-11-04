#pragma once
#include <stdint.h>
#ifndef PET_P4_FLASH_PHASE_TRACE
#define PET_P4_FLASH_PHASE_TRACE 0
#endif
#if PET_P4_FLASH_PHASE_TRACE
/* Fixed bits only: actual selected chip's sector, idle and yield callbacks.
 * No address, chip register access, permission or driver replacement. */
uint32_t pet_flash_phase_coverage(void);
#endif
