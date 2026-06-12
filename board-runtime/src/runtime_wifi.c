#include "runtime_wifi.h"

#include <string.h>

bool br_wifi_credential_valid(const char *value, size_t max_len, bool allow_empty) {
  if (!value) {
    return false;
  }
  size_t len = strlen(value);
  if (len == 0) {
    return allow_empty;
  }
  if (len > max_len) {
    return false;
  }
  for (size_t i = 0; i < len; i += 1) {
    unsigned char ch = (unsigned char) value[i];
    if (ch < 0x20 || ch == 0x7F) {
      return false;
    }
    if (ch == ';' || ch == '$' || ch == '`' || ch == '\\') {
      return false;
    }
  }
  return true;
}
