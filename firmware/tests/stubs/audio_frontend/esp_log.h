#pragma once
/* Evaluate arguments so strict host builds also catch format/signature errors. */
#include <stdio.h>
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
