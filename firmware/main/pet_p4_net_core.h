/*
 * [Input] Topic names, Wi-Fi credentials, authentication nonces, and monotonic millisecond clocks.
 * [Output] Heap-free decisions for the Wi-Fi link: which business topics may cross it, whether
 *          credentials are acceptable, the exact authentication message, failure lockout and
 *          reconnect backoff.
 * [Pos] Platform-independent core beneath pet_p4_network.c; no ESP-IDF, lwIP or JSON dependencies.
 * [Sync] If this file changes, update `pet_p4_net_core.c`, `tests/p4_net_core_test.c`,
 *        `firmware/protocol.md`, and the desktop allowlist in `pc/src-tauri/src/network_transport.rs`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_NET_WIRE_PROTOCOL "pet-net-jsonl-v1"
#define PET_P4_NET_TCP_PORT 38517
#define PET_P4_NET_SSID_MAX 32
#define PET_P4_NET_PASSWORD_MAX 64
#define PET_P4_NET_SECRET_LEN 64
#define PET_P4_NET_NONCE_HEX_LEN 32
#define PET_P4_NET_PROOF_HEX_LEN 64
#define PET_P4_NET_AUTH_TIMEOUT_MS 5000ULL
#define PET_P4_NET_CLIENT_IDLE_MS 15000ULL
#define PET_P4_NET_KEEPALIVE_MS 3000ULL
#define PET_P4_NET_AUTH_FAILURE_LIMIT 5U
#define PET_P4_NET_AUTH_WINDOW_MS 60000ULL
#define PET_P4_NET_AUTH_LOCKOUT_MS 30000ULL
#define PET_P4_NET_CRASH_LOOP_LIMIT 3U

/* Inbound (desktop -> board): status, sessions and key-event plumbing only. Everything else keeps
 * its USB transaction and is refused with `wifi_unsupported`. Mirrors the desktop allowlist. */
bool pet_p4_net_topic_allowed(const char *topic);

/* Outbound events the board may broadcast to an authenticated Wi-Fi client. Audio, media, asset
 * and firmware chatter stay off the LAN even when a physical button starts them. `hello` is not
 * broadcast: its capability set depends on the link and is only produced as a bind/ack reply. */
bool pet_p4_net_broadcast_allowed(const char *topic);

/* Extracts the topic from a serialized `{"topic":"..."` line without parsing JSON.
 * Returns false when the line does not start with that exact shape or the topic does not fit. */
bool pet_p4_net_line_topic(const char *line, char *out, size_t out_size);

/* SSID 1..32 bytes; password empty (open network), 8..63 bytes, or a 64-digit hex PSK. */
bool pet_p4_net_credentials_valid(const char *ssid, const char *password);

/* The pairing secret is a 64-digit lower/upper-case hex string used verbatim as the HMAC key. */
bool pet_p4_net_secret_valid(const char *secret);

/* Writes the exact HMAC input `pet-net-v1|<role>|<board>|<device_nonce>|<client_nonce>`.
 * `role` is "client" for the desktop proof and "server" for the board proof. */
bool pet_p4_net_proof_message(
  const char *role,
  const char *board_device_id,
  const char *device_nonce,
  const char *client_nonce,
  char *out,
  size_t out_size
);

bool pet_p4_net_nonce_valid(const char *nonce);

void pet_p4_net_hex_encode(const uint8_t *data, size_t len, char *out);

/* Compares two NUL-terminated strings without early exit on the first differing byte. */
bool pet_p4_net_ct_equal(const char *left, const char *right);

/* Sliding failure window: PET_P4_NET_AUTH_FAILURE_LIMIT failures within PET_P4_NET_AUTH_WINDOW_MS
 * lock authentication for PET_P4_NET_AUTH_LOCKOUT_MS, which bounds online guessing of the secret. */
typedef struct {
  uint32_t failures;
  uint64_t window_start_ms;
  uint64_t locked_until_ms;
} pet_p4_net_auth_guard_t;

bool pet_p4_net_auth_locked(const pet_p4_net_auth_guard_t *guard, uint64_t now_ms);
void pet_p4_net_auth_failure(pet_p4_net_auth_guard_t *guard, uint64_t now_ms);
void pet_p4_net_auth_success(pet_p4_net_auth_guard_t *guard);

/* 1 s, 2 s, 4 s ... capped at 30 s; attempt is the number of consecutive failures so far. */
uint32_t pet_p4_net_backoff_ms(uint32_t attempt);

/* Maps an ESP-IDF station disconnect reason to a stable error code for the desktop UI. */
const char *pet_p4_net_disconnect_error(int reason);

#ifdef __cplusplus
}
#endif
