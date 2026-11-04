#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*pet_p4_native_protocol_enqueue_fn)(
  const char *line,
  pet_p4_send_line_fn send_line,
  void *reply_ctx,
  void *enqueue_ctx
);

bool pet_p4_native_usb_route_current(void *route);
esp_err_t pet_p4_native_usb_init(
  pet_p4_runtime_state_t *state,
  SemaphoreHandle_t state_mutex,
  pet_p4_native_protocol_enqueue_fn enqueue_protocol,
  void *enqueue_ctx
);
void pet_p4_native_usb_send_json_line(const char *line, void *ctx);
void pet_p4_native_usb_process(unsigned long long now_ms);
/* Caller holds the runtime state mutex. Atomics include frames not yet admitted. */
typedef struct { bool transfer_active, receiving, worker_pending; uint32_t epoch; } pet_p4_native_io_snapshot_t;
pet_p4_native_io_snapshot_t pet_p4_native_usb_io_snapshot(void);

#ifdef __cplusplus
}
#endif
