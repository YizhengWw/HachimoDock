#pragma once
#include "pet_p4_trusted_services.h"

/* Actual P4 providers use esp_timer and the input runtime's existing ADC
 * snapshot only. This does not initialize hardware or advertise bus services.
 * Requires an admitted PERIPHERAL quota and trusted runtime-issued authority;
 * a compiled adapter alone grants nothing. Call from a worker, never an ISR. */
pet_trusted_service_result_t pet_trusted_service_device_read(
  pet_trusted_service_authority_t *authority, uint32_t service,
  pet_trusted_service_snapshot_t *out, pet_trusted_service_diagnostic_t *diagnostic);
