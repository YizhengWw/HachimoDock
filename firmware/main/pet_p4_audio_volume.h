/* Cold master control. Init before publication; all later calls on protocol worker.
 * No PCM, capture, DSP, task creation, or retained transport/request ownership. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define PET_AUDIO_MASTER_VOLUME_ENGINE "p4-master-volume-v1"

typedef struct {
  int volume; /* -1 = unknown; never a requested value represented as applied */
  bool confirmed, persisted, save_pending;
  esp_err_t error, save_error;
} pet_audio_volume_status_t;
unsigned pet_audio_volume_boot_load(void);
esp_err_t pet_audio_volume_attach(esp_codec_dev_handle_t speaker,
  SemaphoreHandle_t stream_lock, i2c_master_bus_handle_t bus);
bool pet_audio_volume_ready(void);
esp_err_t pet_audio_volume_get(pet_audio_volume_status_t *out);
esp_err_t pet_audio_volume_set(unsigned volume, uint64_t now_ms, pet_audio_volume_status_t *out);
bool pet_audio_volume_pending(void);
/* idle is sampled by main outside its state mutex. still_idle is native only,
 * rechecked at physical boundaries; it must not block or touch mutable model. */
void pet_audio_volume_idle_commit(uint64_t now_ms, bool idle, bool (*still_idle)(void));
/* Consumes no JSON ownership. Returns one cJSON-owned serialized result, or NULL.
 * Caller sends/frees once after releasing stream mutex, with original route. */
char *pet_audio_volume_reply(const cJSON *root, const char *board_id, bool available, bool *ok);
