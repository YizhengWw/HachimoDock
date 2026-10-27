#pragma once
/* Single-owner deterministic harness; concurrent boot lock is tested through
 * production builds/source contracts, not by this no-scheduler substitute. */
static inline void vTaskDelay(int ticks) { (void)ticks; }
