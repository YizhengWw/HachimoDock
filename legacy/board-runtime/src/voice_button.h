#ifndef BOARD_RUNTIME_VOICE_BUTTON_H
#define BOARD_RUNTIME_VOICE_BUTTON_H

#include <stdbool.h>
#include <stddef.h>

#define BR_VOICE_BUTTON_TOP_HOLD "top_button.hold"
#define BR_VOICE_BUTTON_ENCODER_HOLD "encoder_button.hold"

const char *br_voice_button_default(void);
bool br_voice_button_normalize(const char *input, char *output, size_t output_size);
bool br_voice_button_is_top_hold(const char *input);
bool br_voice_button_is_encoder_hold(const char *input);

#endif
