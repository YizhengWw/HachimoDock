#pragma once
#include <stdint.h>
typedef void *vad_handle_t;
typedef enum { VAD_MODE_2 = 2 } vad_mode_t;
typedef enum { VAD_SILENCE, VAD_SPEECH } vad_state_t;
vad_handle_t vad_create_with_param(vad_mode_t mode, int rate, int frame_ms, int speech_ms, int silence_ms);
void vad_destroy(vad_handle_t handle);
vad_state_t vad_process_with_trigger(vad_handle_t handle, int16_t *input);
