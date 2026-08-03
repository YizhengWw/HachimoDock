#include "voice_button.h"

#include <ctype.h>
#include <string.h>

#include "runtime_common.h"

const char *br_voice_button_default(void) {
  return BR_VOICE_BUTTON_ENCODER_HOLD;
}

static bool br_voice_button_copy(const char *value, char *output, size_t output_size) {
  if (!output || output_size == 0) {
    return false;
  }
  if (strlen(value) + 1 > output_size) {
    output[0] = '\0';
    return false;
  }
  strcpy(output, value);
  return true;
}

bool br_voice_button_normalize(const char *input, char *output, size_t output_size) {
  char value[96];
  char lowered[96];
  size_t used = 0;

  if (!output || output_size == 0) {
    return false;
  }
  output[0] = '\0';
  if (!input) {
    return false;
  }
  br_normalize_text(input, "", value, sizeof(value));
  if (value[0] == '\0') {
    return false;
  }

  for (size_t i = 0; value[i] != '\0' && used + 1 < sizeof(lowered); i += 1) {
    unsigned char ch = (unsigned char) value[i];
    if (ch == '-' || ch == ' ') {
      lowered[used++] = '_';
    } else {
      lowered[used++] = (char) tolower(ch);
    }
  }
  lowered[used] = '\0';

  if (strcmp(lowered, BR_VOICE_BUTTON_ENCODER_HOLD) == 0 ||
      strcmp(lowered, "encoder_button") == 0 ||
      strcmp(lowered, "rotary_button.hold") == 0 ||
      strcmp(lowered, "rotary_button") == 0 ||
      strcmp(lowered, "knob_button.hold") == 0) {
    return br_voice_button_copy(BR_VOICE_BUTTON_ENCODER_HOLD, output, output_size);
  }
  return false;
}

bool br_voice_button_is_top_hold(const char *input) {
  char normalized[64];
  return br_voice_button_normalize(input, normalized, sizeof(normalized)) &&
         strcmp(normalized, BR_VOICE_BUTTON_TOP_HOLD) == 0;
}

bool br_voice_button_is_encoder_hold(const char *input) {
  char normalized[64];
  return br_voice_button_normalize(input, normalized, sizeof(normalized)) &&
         strcmp(normalized, BR_VOICE_BUTTON_ENCODER_HOLD) == 0;
}
