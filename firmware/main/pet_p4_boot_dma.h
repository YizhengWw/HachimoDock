/* Pre-heap cleanup of unowned AHB DMA inherited from a CPU-only restart.
 * Fixed numeric evidence only; no wire/storage schema or fault-count changes.
 * CPU descriptor words are bounded observations, not cache-coherence proof. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PET_BOOT_DMA_CHANNELS 3U
#define PET_BOOT_DMA_STOP_POLLS 64U
typedef struct {
  uint32_t rx_link_addr,rx_link,rx_state,rx_eof,rx_prefetched,rx_peripheral;
  uint32_t rx_dw0[3],rx_dw0_valid; /* link/eof/prefetched, no chain traversal */
  uint32_t tx_link_addr,tx_link,tx_state,tx_eof,tx_prefetched;
} pet_boot_dma_channel_t;
typedef struct {
  pet_boot_dma_channel_t channels[PET_BOOT_DMA_CHANNELS];
  uint32_t parked_mask;
} pet_boot_dma_state_t;
typedef struct {
  uint32_t version,clock_was_enabled,reset_applied,stop_polls,stopped_mask,complete;
  pet_boot_dma_state_t before,after;
} pet_boot_dma_snapshot_t;
/* Real cross-TU reference keeps the init-hook object in the component archive. */
void pet_boot_dma_link_anchor(void);
bool pet_boot_dma_observe(pet_boot_dma_snapshot_t *out);
