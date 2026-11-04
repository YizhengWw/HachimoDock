/* Paced CONTROL source-alignment isolation, NOT a validated driver/root-cause
 * fix. No allocation, wait, driver call, retry, telemetry or global memcpy hook.
 * Only included in executable paths under the existing default-OFF pacing gate. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pet_p4_uart_pacing.h"

#if PET_P4_UART_PACING_ACTIVE
#define PET_UART_CONTROL_STAGE_BYTES 256U
#define PET_UART_CONTROL_STAGE_ALIGNMENT 64U
_Static_assert(PET_P4_UART_TX_BURST_BYTES <= PET_UART_CONTROL_STAGE_BYTES,
  "control burst exceeds staging bound");
/* Keep a named actual-ELF proof target. Volatile BYTE loads AND stores prevent
 * this new copy being replaced by libc/ROM memcpy. Host behavior alone cannot
 * prove the target instructions or internal DRAM placement. */
static __attribute__((noinline)) const char *pet_uart_control_stage_copy(
    unsigned char *stage, const char *data, size_t bytes) {
  if (!stage || ((uintptr_t)stage % PET_UART_CONTROL_STAGE_ALIGNMENT) != 0
      || bytes > PET_UART_CONTROL_STAGE_BYTES || (!data && bytes)) return NULL;
  volatile unsigned char *dst = stage;
  const volatile unsigned char *src = (const volatile unsigned char *)data;
  for (size_t i = 0; i < bytes; ++i) dst[i] = src[i];
  return (const char *)stage;
}
#endif
