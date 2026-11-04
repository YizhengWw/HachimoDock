/* Default-off native phase scopes. Never call in an ISR/critical section;
 * no task, grant, user callback, IO, logging, timeouts or watchdog changes. */
#pragma once
#include "pet_p4_reset_record.h"
#if PET_P4_RESET_PHASE_TRACE
typedef enum {
  PET_RESET_MAIN, PET_RESET_PROTOCOL, PET_RESET_SAVE, PET_RESET_UART_RX
} pet_reset_actor_t;
typedef enum {
  PET_PHASE_IDLE, PET_PHASE_BOOT, PET_PHASE_STATE_WAIT, PET_PHASE_STATE_HELD,
  PET_PHASE_PROGRAM_STEP, PET_PHASE_PROGRAM_PUBLICATION, PET_PHASE_SAVE,
  PET_PHASE_TX_WAIT, PET_PHASE_TX_HELD, PET_PHASE_LOG_WAIT, PET_PHASE_LOG_HELD,
  PET_PHASE_RENDER, PET_PHASE_FILE_DATA, PET_PHASE_FILE_SYNC, PET_PHASE_FILE_CLOSE,
  PET_PHASE_AUDIO_PROCESS, PET_PHASE_RX_WAIT, PET_PHASE_RX_DISPATCH, PET_PHASE_STORAGE_UNLOCKED,
  PET_PHASE_DIAGNOSTIC_FS_INFO, PET_PHASE_DIAGNOSTIC_ALLOC,
  PET_PHASE_ENQUEUE_ALLOC, PET_PHASE_ENQUEUE_COPY, PET_PHASE_ENQUEUE_QUEUE,
  PET_PHASE_HAL_READ_META, PET_PHASE_HAL_READ_NATIVE,
  PET_PHASE_HAL_WRITE_META, PET_PHASE_HAL_WRITE_NATIVE,
  PET_PHASE_HAL_ERASE_META, PET_PHASE_HAL_ERASE_NATIVE,
  PET_PHASE_PARTITION_ERASE, PET_PHASE_FLASH_ERASE, PET_PHASE_FLASH_YIELD,
  PET_PHASE_FLASH_SECTOR, PET_PHASE_FLASH_WAIT_IDLE, PET_PHASE_FLASH_CACHE_FLUSH,
  PET_PHASE_DIAGNOSTIC_IMAGE_STATE,
  PET_PHASE_COUNT
} pet_reset_phase_t;
typedef struct {uintptr_t owner;uint32_t serial,previous_serial;uint8_t actor,depth,previous;bool active;} pet_reset_phase_scope_t;
void pet_reset_phase_boot(void);
bool pet_reset_phase_bind(pet_reset_actor_t actor);
void pet_reset_phase_set(pet_reset_phase_t phase);
pet_reset_phase_scope_t pet_reset_phase_enter(pet_reset_phase_t phase);
void pet_reset_phase_leave(pet_reset_phase_scope_t *scope);
/* Previous valid SAME-BUILD records cached at boot, never live racing RTC
 * reads. Cycle counters wrap per core, not wall/cross-core chronology. */
bool pet_reset_phase_previous(pet_reset_record_snapshot_t *out,uint32_t *current_errors);
#define PET_RESET_PHASE_SCOPE(name,phase) \
  pet_reset_phase_scope_t name __attribute__((cleanup(pet_reset_phase_leave)))=pet_reset_phase_enter(phase)
#endif
