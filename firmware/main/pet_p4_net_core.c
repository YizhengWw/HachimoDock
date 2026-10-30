/*
 * [Input] See pet_p4_net_core.h.
 * [Output] Heap-free Wi-Fi link policy shared by the firmware and its host regression tests.
 * [Pos] Platform-independent core beneath pet_p4_network.c.
 * [Sync] If this file changes, update `pet_p4_net_core.h`, `tests/p4_net_core_test.c`, and `protocol.md`.
 */

#include "pet_p4_net_core.h"

#include <stdio.h>
#include <string.h>

static const char *const k_inbound_topics[] = {
  "ack", "bind", "system/heartbeat", "speech/text", "session/current", "control/screen-page",
  "control/command", "stats/update", "runtime/stats",
};
static const char *const k_inbound_prefixes[] = {"state/", "diagnostics/", "network/"};

/* `input/` carries physical key, joystick and touch events plus the button-mapping echo. */
static const char *const k_broadcast_prefixes[] = {
  "input/", "session/", "protocol/", "diagnostics/", "network/",
};

static bool has_prefix(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool pet_p4_net_topic_allowed(const char *topic) {
  if (!topic || !topic[0]) return false;
  for (size_t i = 0; i < sizeof(k_inbound_topics) / sizeof(k_inbound_topics[0]); ++i) {
    if (strcmp(topic, k_inbound_topics[i]) == 0) return true;
  }
  for (size_t i = 0; i < sizeof(k_inbound_prefixes) / sizeof(k_inbound_prefixes[0]); ++i) {
    if (has_prefix(topic, k_inbound_prefixes[i])) return true;
  }
  return false;
}

bool pet_p4_net_broadcast_allowed(const char *topic) {
  if (!topic || !topic[0]) return false;
  for (size_t i = 0; i < sizeof(k_broadcast_prefixes) / sizeof(k_broadcast_prefixes[0]); ++i) {
    if (has_prefix(topic, k_broadcast_prefixes[i])) return true;
  }
  return strcmp(topic, "button-config-ack") == 0;
}

bool pet_p4_net_line_topic(const char *line, char *out, size_t out_size) {
  static const char prefix[] = "{\"topic\":\"";
  if (!line || !out || out_size < 2 || strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;
  const char *start = line + sizeof(prefix) - 1;
  size_t len = 0;
  while (start[len] && start[len] != '"') {
    if (start[len] == '\\') return false; /* topics never contain escapes */
    len += 1;
  }
  if (start[len] != '"' || len == 0 || len >= out_size) return false;
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

static bool is_hex_text(const char *text, size_t length) {
  if (!text || strlen(text) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    char ch = text[i];
    bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!hex) return false;
  }
  return true;
}

bool pet_p4_net_credentials_valid(const char *ssid, const char *password) {
  if (!ssid || !password) return false;
  size_t ssid_len = strlen(ssid);
  if (ssid_len == 0 || ssid_len > PET_P4_NET_SSID_MAX) return false;
  size_t password_len = strlen(password);
  if (password_len == 0) return true;
  if (password_len == PET_P4_NET_PASSWORD_MAX) return is_hex_text(password, PET_P4_NET_PASSWORD_MAX);
  return password_len >= 8 && password_len <= 63;
}

bool pet_p4_net_secret_valid(const char *secret) {
  return is_hex_text(secret, PET_P4_NET_SECRET_LEN);
}

bool pet_p4_net_nonce_valid(const char *nonce) {
  if (!nonce) return false;
  size_t length = strlen(nonce);
  return length >= 16 && length <= 64 && is_hex_text(nonce, length);
}

bool pet_p4_net_proof_message(
  const char *role,
  const char *board_device_id,
  const char *device_nonce,
  const char *client_nonce,
  char *out,
  size_t out_size
) {
  if (!role || !board_device_id || !device_nonce || !client_nonce || !out) return false;
  if (strcmp(role, "client") != 0 && strcmp(role, "server") != 0) return false;
  int written = snprintf(out, out_size, "pet-net-v1|%s|%s|%s|%s", role, board_device_id, device_nonce, client_nonce);
  return written > 0 && (size_t) written < out_size;
}

void pet_p4_net_hex_encode(const uint8_t *data, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

bool pet_p4_net_ct_equal(const char *left, const char *right) {
  if (!left || !right) return false;
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  size_t span = left_len > right_len ? left_len : right_len;
  unsigned diff = (unsigned) (left_len ^ right_len);
  for (size_t i = 0; i < span; ++i) {
    unsigned a = i < left_len ? (unsigned char) left[i] : 0;
    unsigned b = i < right_len ? (unsigned char) right[i] : 0;
    diff |= a ^ b;
  }
  return diff == 0;
}

bool pet_p4_net_auth_locked(const pet_p4_net_auth_guard_t *guard, uint64_t now_ms) {
  return guard && guard->locked_until_ms != 0 && now_ms < guard->locked_until_ms;
}

void pet_p4_net_auth_failure(pet_p4_net_auth_guard_t *guard, uint64_t now_ms) {
  if (!guard) return;
  if (guard->failures == 0 || now_ms - guard->window_start_ms > PET_P4_NET_AUTH_WINDOW_MS) {
    guard->failures = 0;
    guard->window_start_ms = now_ms;
  }
  guard->failures += 1;
  if (guard->failures >= PET_P4_NET_AUTH_FAILURE_LIMIT) {
    guard->locked_until_ms = now_ms + PET_P4_NET_AUTH_LOCKOUT_MS;
    guard->failures = 0;
  }
}

void pet_p4_net_auth_success(pet_p4_net_auth_guard_t *guard) {
  if (!guard) return;
  guard->failures = 0;
  guard->locked_until_ms = 0;
}

uint32_t pet_p4_net_backoff_ms(uint32_t attempt) {
  uint32_t exponent = attempt > 5 ? 5 : attempt;
  return 1000U << exponent > 30000U ? 30000U : 1000U << exponent;
}

const char *pet_p4_net_disconnect_error(int reason) {
  switch (reason) {
    case 201: /* NO_AP_FOUND */
    case 210: /* NO_AP_FOUND_W_COMPATIBLE_SECURITY */
    case 211: /* NO_AP_FOUND_IN_AUTHMODE_THRESHOLD */
    case 212: /* NO_AP_FOUND_IN_RSSI_THRESHOLD */
      return "ap_not_found";
    case 14:  /* MIC_FAILURE */
    case 15:  /* 4WAY_HANDSHAKE_TIMEOUT */
    case 16:  /* GROUP_KEY_UPDATE_TIMEOUT */
    case 17:  /* IE_IN_4WAY_DIFFERS */
    case 23:  /* 802_1X_AUTH_FAILED */
    case 202: /* AUTH_FAIL */
    case 204: /* HANDSHAKE_TIMEOUT */
      return "wrong_password";
    case 0:
      return "";
    default:
      return "disconnected";
  }
}
