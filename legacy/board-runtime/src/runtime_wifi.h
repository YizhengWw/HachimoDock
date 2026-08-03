#ifndef BOARD_RUNTIME_WIFI_H
#define BOARD_RUNTIME_WIFI_H

#include <stdbool.h>
#include <stddef.h>

/* Validate a Wi-Fi SSID or PSK supplied by the desktop client over USB.
 *
 * Returns true iff `value` is non-NULL, its length is in [1..max_len]
 * (or [0..max_len] when allow_empty), contains no control chars / NUL,
 * and contains none of the shell metacharacters that are dangerous inside
 * the double-quoted arguments to `wpa_passphrase` invoked by
 * board-sta-apply.sh (semicolon, dollar sign, backtick, backslash).
 *
 * `max_len` is the maximum payload length in bytes (excluding NUL). */
bool br_wifi_credential_valid(const char *value, size_t max_len, bool allow_empty);

#endif /* BOARD_RUNTIME_WIFI_H */
