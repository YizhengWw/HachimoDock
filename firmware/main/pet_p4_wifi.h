/*
 * [Input] Wi-Fi credentials from NVS, optional PC config commands, and the
 *         shared JSONL protocol callbacks from pet_p4_main.c.
 * [Output] ESP32-C6 hosted STA connection, a single-client TCP transport on
 *          port 3232, and wifi/config + wifi/status command replies.
 * [Pos] Optional Wi-Fi transport for the ESP32-P4 desk pet.
 * [Sync] If this file changes, update firmware/.folder.md and protocol.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pet_p4_wifi_rx_fn)(const uint8_t *data, size_t len, void *ctx);
typedef void (*pet_p4_wifi_client_fn)(void *ctx);

typedef struct {
  pet_p4_wifi_rx_fn on_bytes;
  pet_p4_wifi_client_fn on_client_connected;
  void *ctx;
} pet_p4_wifi_callbacks_t;

/* Create the boot task; it loads NVS credentials and connects when present. */
esp_err_t pet_p4_wifi_init(const pet_p4_wifi_callbacks_t *callbacks);

/* True when the TCP server has an accepted PC client. */
bool pet_p4_wifi_client_connected(void);

/* True when an IP address has been obtained. */
bool pet_p4_wifi_ready(void);

/* C6 co-processor firmware version as reported by esp_hosted (e.g. "2.12.11"). */
void pet_p4_wifi_c6_version(char *out, size_t out_len);

/* True when the C6 firmware is compatible with this host's esp_hosted. */
bool pet_p4_wifi_tcp_compatible(void);

/* Send one JSON line (newline appended) to the active Wi-Fi client. */
bool pet_p4_wifi_send_line(const char *line);

/* Send arbitrary bytes to the active Wi-Fi client (for streamed chunks). */
bool pet_p4_wifi_send_bytes(const char *data, size_t len);

/* Handle wifi/config and wifi/status protocol lines. Returns true if handled. */
bool pet_p4_wifi_handle_command(
  const char *line,
  pet_p4_send_line_fn send_line,
  void *reply_ctx
);

#ifdef __cplusplus
}
#endif
