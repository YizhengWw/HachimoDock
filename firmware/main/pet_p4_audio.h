#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_AUDIO_SAMPLE_RATE 16000
#define PET_P4_AUDIO_CHANNELS 1
#define PET_P4_AUDIO_BITS_PER_SAMPLE 16
#define PET_P4_AUDIO_FRAME_MS 20

esp_err_t pet_p4_audio_init(
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_audio_ready(void);
/* Route capture to the client that enabled it, not every enumerated USB link. */
void pet_p4_audio_set_transport(pet_p4_send_line_fn send_line, void *ctx);
bool pet_p4_audio_enabled(void);
bool pet_p4_audio_active(void);
const char *pet_p4_audio_codec_name(void);
bool pet_p4_audio_playback_ready(void);
bool pet_p4_audio_playback_active(void);
esp_err_t pet_p4_audio_set_enabled(bool enabled);
esp_err_t pet_p4_audio_capture_start(bool session_queue_empty);
esp_err_t pet_p4_audio_capture_stop(void);

/* 实时对话：PC 经 audio/conversation 打开后麦克风持续采集上行（不受
 * 30 s 上限）；AEC/VAD 可用时支持双向语音，旧客户端半双工播放期间不上行。
 * PC 经 audio/play_begin/chunk/end/flush
 * 流式下发 16 kHz PCM，设备缓冲约 200 ms 后起播。 */
#define PET_P4_AUDIO_STREAM_CHUNK_MAX 4096
#define PET_P4_AUDIO_STREAM_CHUNK_B64_MAX (((PET_P4_AUDIO_STREAM_CHUNK_MAX + 2) / 3) * 4 + 4)
esp_err_t pet_p4_audio_conversation_set(bool enabled, bool half_duplex);
bool pet_p4_audio_conversation_active(void);
esp_err_t pet_p4_audio_stream_begin(const char *session_id);
bool pet_p4_audio_stream_matches(const char *session_id);
esp_err_t pet_p4_audio_stream_push(const uint8_t *pcm, size_t length);
esp_err_t pet_p4_audio_stream_end(void);
void pet_p4_audio_stream_flush(void);
unsigned int pet_p4_audio_stream_played_bytes(void);
bool pet_p4_audio_stream_playing(void);
/* Metadata-only device diagnostic events: no PCM or spoken text. */
void pet_p4_audio_diagnostic(const char *event, const char *session_id, size_t bytes, bool ok);
void pet_p4_audio_process(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
void pet_p4_audio_send_status(void);

#ifdef __cplusplus
}
#endif
