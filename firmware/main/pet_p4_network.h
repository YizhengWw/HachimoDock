/*
 * [Input] Wi-Fi credentials and a pairing secret delivered over USB, plus the ESP32-C6 co-processor
 *         reached through ESP-Hosted over SDIO.
 * [Output] A lazily started station connection, one mutually authenticated TCP JSONL client, UDP
 *          discovery replies, and `network/status` snapshots; protocol bytes are handed to the
 *          existing execution queue untouched.
 * [Pos] ESP32-P4 Wi-Fi transport node beside the UART, USB Serial/JTAG and native USB links.
 * [Sync] If this file changes, update `firmware/.folder.md`, `protocol.md`, and `pet_p4_main.c`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "pet_p4_net_core.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *board_device_id;
  const char *firmware_version;
  /* Called from the network task with bytes of an authenticated client only. */
  void (*on_bytes)(const uint8_t *data, size_t len, void *ctx);
  /* Called when the authenticated client attaches or detaches. */
  void (*on_client)(bool attached, void *ctx);
  void *ctx;
} pet_p4_network_config_t;

/* Loads stored credentials and starts the supervisor task. The radio and the ESP-Hosted transport
 * stay untouched until credentials exist, so a USB-only board behaves exactly as before. */
esp_err_t pet_p4_network_init(const pet_p4_network_config_t *config);

/* Handles `network/config`, `network/clear` and `network/query`. `via_wifi` marks requests that
 * arrived on the LAN: provisioning and clearing are refused there because they carry or destroy
 * the credentials that make the LAN link reachable. Returns false for other topics. */
bool pet_p4_network_handle_topic(
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx,
  bool via_wifi
);

bool pet_p4_network_client_ready(void);

/* Sends one complete line (newline appended) to the authenticated client. Returns false when no
 * client is attached or the peer cannot keep up, in which case the client is dropped. */
bool pet_p4_network_send_line(const char *line);

/* Contributes `capabilities.network` to hello (supported, configured, state, port). */
void pet_p4_network_describe(cJSON *capabilities);

#ifdef __cplusplus
}
#endif
