#include "runtime_common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "runtime_json.h"

long long br_now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return ((long long) tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

void br_sleep_ms(int milliseconds) {
  struct timespec req;
  if (milliseconds <= 0) {
    return;
  }
  req.tv_sec = milliseconds / 1000;
  req.tv_nsec = (long) (milliseconds % 1000) * 1000000L;
  nanosleep(&req, NULL);
}

static bool br_copy_text(const char *text, char *output, size_t output_size) {
  size_t length;
  if (!output || output_size == 0) {
    return false;
  }
  if (!text) {
    output[0] = '\0';
    return false;
  }
  length = strlen(text);
  if (length + 1 > output_size) {
    return false;
  }
  memcpy(output, text, length + 1);
  return true;
}

char *br_trim(char *text) {
  char *end;
  if (!text) {
    return text;
  }
  while (*text && isspace((unsigned char) *text)) {
    text += 1;
  }
  if (*text == '\0') {
    return text;
  }
  end = text + strlen(text) - 1;
  while (end > text && isspace((unsigned char) *end)) {
    *end = '\0';
    end -= 1;
  }
  return text;
}

bool br_normalize_text(const char *input, const char *fallback, char *output, size_t output_size) {
  char temp[BR_MAX_TEXT];
  char *trimmed;
  const char *source = input;

  if (!output || output_size == 0) {
    return false;
  }
  if (input && input == output) {
    if (!br_copy_text(input, temp, sizeof(temp))) {
      return false;
    }
    source = temp;
  }
  output[0] = '\0';

  if (!source || *source == '\0') {
    return br_copy_text(fallback ? fallback : "", output, output_size);
  }
  if (source != temp && !br_copy_text(source, temp, sizeof(temp))) {
    return false;
  }
  trimmed = br_trim(temp);
  if (*trimmed == '\0') {
    return br_copy_text(fallback ? fallback : "", output, output_size);
  }
  /* Truncate to fit rather than silently drop: copy at most output_size-1 bytes.
     CRITICAL: walk back to a UTF-8 codepoint boundary before terminating, otherwise
     a CJK character split mid-sequence renders as tofu / ? on the device font cache.
     - Continuation byte: 10xxxxxx (0x80..0xBF) — & 0xC0 == 0x80
     - Lead byte:         11xxxxxx (0xC0..0xFF) — & 0xC0 == 0xC0
     - ASCII byte:        0xxxxxxx                — & 0x80 == 0 */
  {
    size_t trimmed_len = strlen(trimmed);
    if (trimmed_len + 1 > output_size) {
      size_t copy_len = output_size - 1;
      memcpy(output, trimmed, copy_len);
      /* Step 1: if the next byte in the SOURCE (trimmed[copy_len]) is a continuation
         byte, we cut in the middle of a multi-byte sequence — back up until we
         reach a non-continuation byte (lead byte of the broken sequence, or ASCII). */
      while (copy_len > 0 && (((unsigned char) trimmed[copy_len]) & 0xC0) == 0x80) {
        copy_len -= 1;
      }
      /* Step 2: if the last byte we'd keep is itself a multi-byte LEAD byte, the
         sequence it starts is incomplete — drop the lead byte too. */
      if (copy_len > 0 && (((unsigned char) trimmed[copy_len - 1]) & 0xC0) == 0xC0) {
        copy_len -= 1;
      }
      output[copy_len] = '\0';
      return true;
    }
  }
  return br_copy_text(trimmed, output, output_size);
}

bool br_normalize_topic_part(
  const char *input,
  const char *fallback,
  char *output,
  size_t output_size
) {
  char temp[BR_MAX_TEXT];
  char *trimmed;
  const char *source = input;
  size_t used = 0;
  bool last_dash = false;

  if (!output || output_size == 0) {
    return false;
  }
  if (input && input == output) {
    if (!br_copy_text(input, temp, sizeof(temp))) {
      return false;
    }
    source = temp;
  }
  output[0] = '\0';

  if (!source || *source == '\0') {
    return br_copy_text(fallback ? fallback : "", output, output_size);
  }
  if (source != temp && !br_copy_text(source, temp, sizeof(temp))) {
    return false;
  }
  trimmed = br_trim(temp);
  while (*trimmed == '/') {
    trimmed += 1;
  }
  while (*trimmed != '\0') {
    char ch = *trimmed;
    if (isalnum((unsigned char) ch) || ch == '.' || ch == '_' || ch == '-') {
      if (used + 1 >= output_size) {
        return false;
      }
      output[used++] = ch;
      last_dash = (ch == '-');
    } else if (!last_dash) {
      if (used + 1 >= output_size) {
        return false;
      }
      output[used++] = '-';
      last_dash = true;
    }
    trimmed += 1;
  }

  while (used > 0 && (output[used - 1] == '-' || output[used - 1] == '/')) {
    used -= 1;
  }
  output[used] = '\0';
  if (used == 0) {
    return br_copy_text(fallback ? fallback : "", output, output_size);
  }
  return true;
}

bool br_read_text_file(const char *path, char *output, size_t output_size) {
  FILE *file;
  size_t read_size;

  if (!path || !output || output_size == 0) {
    return false;
  }
  output[0] = '\0';
  file = fopen(path, "rb");
  if (!file) {
    return false;
  }
  read_size = fread(output, 1, output_size - 1, file);
  fclose(file);
  output[read_size] = '\0';
  return true;
}

bool br_atomic_write_text(const char *path, const char *value) {
  char temp_path[BR_MAX_PATH];
  FILE *file;

  if (!path || !value) {
    return false;
  }
  if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", path) >= (int) sizeof(temp_path)) {
    return false;
  }
  file = fopen(temp_path, "wb");
  if (!file) {
    return false;
  }
  if (fputs(value, file) == EOF) {
    fclose(file);
    unlink(temp_path);
    return false;
  }
  fclose(file);
  if (rename(temp_path, path) != 0) {
    unlink(temp_path);
    return false;
  }
  return true;
}

unsigned long long br_fnv1a64_update(
  unsigned long long checksum,
  const unsigned char *data,
  size_t data_size
) {
  size_t i;
  if (!data) {
    return checksum;
  }
  for (i = 0; i < data_size; i += 1) {
    checksum ^= (unsigned long long) data[i];
    checksum *= BR_FNV1A64_PRIME;
  }
  return checksum;
}

void br_fnv1a64_hex(unsigned long long checksum, char *output, size_t output_size) {
  if (!output || output_size == 0) {
    return;
  }
  snprintf(output, output_size, "%016llx", checksum);
}

bool br_apply_payload_write(
  const char *root_dir,
  const char *requested_path,
  const char *content,
  const char **error_msg
) {
  static const char *const ALLOWED[] = {
    ".stats-display",
    ".current-speech",
    ".screen-page",
    NULL,
  };
  if (!root_dir || !requested_path || !content) {
    if (error_msg) *error_msg = "missing argument";
    return false;
  }
  bool allowed = false;
  for (const char *const *p = ALLOWED; *p; ++p) {
    if (strcmp(requested_path, *p) == 0) {
      allowed = true;
      break;
    }
  }
  if (!allowed) {
    if (error_msg) *error_msg = "path not in whitelist";
    return false;
  }
  char full_path[BR_MAX_PATH];
  if (!br_safe_join(root_dir, requested_path, full_path, sizeof(full_path))) {
    if (error_msg) *error_msg = "safe_join failed";
    return false;
  }
  if (!br_atomic_write_text(full_path, content)) {
    if (error_msg) *error_msg = "atomic_write failed";
    return false;
  }
  return true;
}

bool br_read_device_id_json(const char *path, char *output, size_t output_size) {
  char buffer[2048];
  br_json_token tokens[128];
  int count;
  int value_index;

  if (!br_read_text_file(path, buffer, sizeof(buffer))) {
    return false;
  }
  count = br_json_parse(buffer, strlen(buffer), tokens, (int) (sizeof(tokens) / sizeof(tokens[0])));
  if (count <= 0 || tokens[0].type != BR_JSON_OBJECT) {
    return false;
  }
  value_index = br_json_find_key(buffer, tokens, count, 0, "deviceId");
  if (value_index >= 0 && br_json_token_to_string(buffer, &tokens[value_index], output, output_size)) {
    return true;
  }
  value_index = br_json_find_key(buffer, tokens, count, 0, "desktopDeviceId");
  if (value_index >= 0 && br_json_token_to_string(buffer, &tokens[value_index], output, output_size)) {
    return true;
  }
  return false;
}

bool br_get_hostname_text(char *output, size_t output_size) {
  char hostname[256];
  if (!output || output_size == 0) {
    return false;
  }
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    return false;
  }
  hostname[sizeof(hostname) - 1] = '\0';
  return br_normalize_text(hostname, "", output, output_size);
}

bool br_get_first_lan_ipv4(char *output, size_t output_size) {
  struct ifaddrs *interfaces = NULL;
  struct ifaddrs *entry = NULL;

  if (!output || output_size == 0) {
    return false;
  }
  output[0] = '\0';

  if (getifaddrs(&interfaces) != 0) {
    return false;
  }

  for (entry = interfaces; entry; entry = entry->ifa_next) {
    struct sockaddr_in *addr;
    const char *text;

    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((entry->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    addr = (struct sockaddr_in *) entry->ifa_addr;
    text = inet_ntoa(addr->sin_addr);
    if (!text || strncmp(text, "169.254.", 8) == 0) {
      continue;
    }
    br_copy_text(text, output, output_size);
    freeifaddrs(interfaces);
    return true;
  }

  freeifaddrs(interfaces);
  return false;
}

const char *br_content_type(const char *path) {
  const char *ext = strrchr(path ? path : "", '.');
  if (!ext) {
    return "application/octet-stream";
  }
  if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
  if (strcmp(ext, ".js") == 0) return "application/javascript; charset=utf-8";
  if (strcmp(ext, ".css") == 0) return "text/css; charset=utf-8";
  if (strcmp(ext, ".json") == 0) return "application/json; charset=utf-8";
  if (strcmp(ext, ".mp4") == 0) return "video/mp4";
  if (strcmp(ext, ".png") == 0) return "image/png";
  if (strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".jpg") == 0) return "image/jpeg";
  if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
  return "application/octet-stream";
}

bool br_safe_join(const char *base_dir, const char *request_path, char *output, size_t output_size) {
  char local[BR_MAX_PATH];
  char clean[BR_MAX_PATH];
  size_t clean_used = 0;

  if (!base_dir || !request_path || !output || output_size == 0) {
    return false;
  }
  if (!br_copy_text(request_path, local, sizeof(local))) {
    return false;
  }

  char *cursor = local;
  if (*cursor == '/') {
    cursor += 1;
  }
  while (*cursor) {
    char ch = *cursor;
    if (ch == '?') {
      break;
    }
    if (ch == '%' &&
        isxdigit((unsigned char) cursor[1]) &&
        isxdigit((unsigned char) cursor[2])) {
      unsigned int value = 0;
      sscanf(cursor + 1, "%2x", &value);
      ch = (char) value;
      cursor += 2;
    }
    if (clean_used + 1 >= sizeof(clean)) {
      return false;
    }
    clean[clean_used++] = ch;
    cursor += 1;
  }
  clean[clean_used] = '\0';

  if (strstr(clean, "..")) {
    return false;
  }
  return snprintf(output, output_size, "%s/%s", base_dir, clean) < (int) output_size;
}

bool br_parse_mqtt_url(const char *url, br_mqtt_endpoint *endpoint) {
  char temp[256];
  char *host;
  char *port_text;

  if (!url || !endpoint) {
    return false;
  }
  memset(endpoint, 0, sizeof(*endpoint));
  if (!br_copy_text(url, temp, sizeof(temp))) {
    return false;
  }
  host = temp;
  if (strncmp(host, "mqtt://", 7) == 0) {
    host += 7;
  } else if (strncmp(host, "tcp://", 6) == 0) {
    host += 6;
  } else {
    return false;
  }

  port_text = strrchr(host, ':');
  if (port_text) {
    *port_text = '\0';
    port_text += 1;
  }

  if (!br_copy_text(host, endpoint->host, sizeof(endpoint->host))) {
    return false;
  }
  endpoint->port = port_text ? atoi(port_text) : 1883;
  if (endpoint->port <= 0 || endpoint->port > 65535) {
    endpoint->port = 1883;
  }
  return true;
}

void br_iso8601_now(char *output, size_t output_size) {
  time_t now;
  struct tm value;
  struct timeval tv;

  if (!output || output_size == 0) {
    return;
  }
  gettimeofday(&tv, NULL);
  now = tv.tv_sec;
  gmtime_r(&now, &value);
  strftime(output, output_size, "%Y-%m-%dT%H:%M:%S", &value);
  snprintf(output + strlen(output), output_size - strlen(output), ".%03dZ", (int) (tv.tv_usec / 1000));
}

int br_snprintf_append(char *buffer, size_t size, size_t *used, const char *format, ...) {
  va_list args;
  int written;
  if (!buffer || !used || !format || *used >= size) {
    return -1;
  }
  va_start(args, format);
  written = vsnprintf(buffer + *used, size - *used, format, args);
  va_end(args);
  if (written < 0 || *used + (size_t) written >= size) {
    return -1;
  }
  *used += (size_t) written;
  return 0;
}

void br_json_escape_append(char *buffer, size_t size, size_t *used, const char *text) {
  const unsigned char *cursor = (const unsigned char *) (text ? text : "");
  while (*cursor && *used + 2 < size) {
    switch (*cursor) {
      case '\\':
        br_snprintf_append(buffer, size, used, "\\\\");
        break;
      case '"':
        br_snprintf_append(buffer, size, used, "\\\"");
        break;
      case '\n':
        br_snprintf_append(buffer, size, used, "\\n");
        break;
      case '\r':
        br_snprintf_append(buffer, size, used, "\\r");
        break;
      case '\t':
        br_snprintf_append(buffer, size, used, "\\t");
        break;
      default:
        if (*cursor < 0x20) {
          br_snprintf_append(buffer, size, used, "\\u%04x", *cursor);
        } else {
          buffer[*used] = (char) *cursor;
          *used += 1;
          buffer[*used] = '\0';
        }
        break;
    }
    cursor += 1;
  }
}
