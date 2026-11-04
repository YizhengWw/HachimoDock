/* Native cooperative execution window, NOT component authority or a task.
 * The pure-compute parent only tries; non-realtime drained control TX waits.
 * Audio/media never enter this gate. Do not hold it across model/FS/driver
 * waits, callbacks or sends from the pure-compute side. No priority inheritance.
 */
#pragma once
#include <stdbool.h>
#ifndef PET_P4_NATIVE_WORK_WINDOW
#define PET_P4_NATIVE_WORK_WINDOW 0
#endif
#if PET_P4_NATIVE_WORK_WINDOW
bool pet_native_work_window_init(void);
bool pet_native_work_window_try_begin(void);
bool pet_native_work_window_control_begin(void);
bool pet_native_work_window_end(void);
#else
static inline bool pet_native_work_window_init(void){return true;}
static inline bool pet_native_work_window_try_begin(void){return true;}
static inline bool pet_native_work_window_control_begin(void){return true;}
static inline bool pet_native_work_window_end(void){return true;}
#endif
