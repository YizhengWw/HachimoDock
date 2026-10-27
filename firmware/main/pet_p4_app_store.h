#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Private device saves, never included in exported component packages. */
#define PET_APP_SAVE_BYTES 1024
bool pet_app_store_init(void);
bool pet_app_store_read(const char *id, char out[PET_APP_SAVE_BYTES]);
bool pet_app_store_update(const char *id, const char *json);
void pet_app_store_forget(const char *id);
