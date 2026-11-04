/* Private bounded native recorder measurement, not a component capability. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_RESET_PHASE_BENCHMARK
#define PET_P4_RESET_PHASE_BENCHMARK 0
#endif
#if PET_P4_RESET_PHASE_BENCHMARK
typedef struct {
  uint64_t total_cycles,total_wall_us;
  uint32_t samples,min_cycles,max_cycles,min_wall_us,max_wall_us,clock_hz;
  bool valid;
} pet_reset_cost_t;
/* Native app_main once, before other owners/tasks. No file/model/audio IO. */
void pet_reset_cost_boot(void);
bool pet_reset_cost_observe(pet_reset_cost_t *out);
#endif
