#ifndef BOARD_RUNTIME_COMMON_H
#define BOARD_RUNTIME_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BR_MAX_PATH 1024
#define BR_MAX_TOPIC 256
#define BR_MAX_TEXT 512
#define BR_MAX_JSON 131072 /* 128KB — supports large base64 asset chunks */
#define BR_FNV1A64_OFFSET 14695981039346656037ULL
#define BR_FNV1A64_PRIME 1099511628211ULL

typedef struct {
  char host[128];
  int port;
} br_mqtt_endpoint;

long long br_now_ms(void);
void br_sleep_ms(int milliseconds);

bool br_normalize_topic_part(
  const char *input,
  const char *fallback,
  char *output,
  size_t output_size
);
/**
 * Trim leading/trailing ASCII whitespace from `input`, fall back to `fallback` if
 * empty/null, and write the result into `output` (NUL-terminated).
 *
 * Truncation contract: if the (trimmed) source exceeds `output_size - 1` bytes,
 * the output is truncated to fit AND the truncation point is rolled back to the
 * nearest UTF-8 codepoint boundary so partial multi-byte sequences are never
 * emitted. Callers can therefore safely pass `output` straight to the glyph
 * cache / font renderer without risking mojibake on the cut. Truncation may
 * yield an empty string when even the first codepoint exceeds the buffer.
 *
 * Returns false only on null `output` / zero `output_size` / internal copy
 * failure; truncation itself is not an error and returns true.
 */
bool br_normalize_text(const char *input, const char *fallback, char *output, size_t output_size);
bool br_read_text_file(const char *path, char *output, size_t output_size);
bool br_atomic_write_text(const char *path, const char *value);
unsigned long long br_fnv1a64_update(
  unsigned long long checksum,
  const unsigned char *data,
  size_t data_size
);
void br_fnv1a64_hex(unsigned long long checksum, char *output, size_t output_size);
bool br_read_device_id_json(const char *path, char *output, size_t output_size);
bool br_get_hostname_text(char *output, size_t output_size);
bool br_get_first_lan_ipv4(char *output, size_t output_size);
const char *br_content_type(const char *path);
bool br_safe_join(const char *base_dir, const char *request_path, char *output, size_t output_size);
/**
 * Safely writes a small text payload to a whitelisted device file.
 * Allowed paths (relative to root_dir): ".stats-display", ".current-speech", ".screen-page".
 * Returns false if path not in whitelist, safe_join fails, or atomic write fails.
 * Out param error_msg may be NULL; if non-NULL, receives a static string on failure.
 */
bool br_apply_payload_write(
  const char *root_dir,
  const char *requested_path,
  const char *content,
  const char **error_msg
);
bool br_parse_mqtt_url(const char *url, br_mqtt_endpoint *endpoint);
void br_iso8601_now(char *output, size_t output_size);
int br_snprintf_append(char *buffer, size_t size, size_t *used, const char *format, ...);
void br_json_escape_append(char *buffer, size_t size, size_t *used, const char *text);
char *br_trim(char *text);

#endif
