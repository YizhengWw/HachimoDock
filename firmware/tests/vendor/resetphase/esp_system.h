#pragma once
typedef enum {ESP_RST_UNKNOWN=0,ESP_RST_POWERON=1,ESP_RST_SW=3,ESP_RST_PANIC=4,
    ESP_RST_INT_WDT=5,ESP_RST_TASK_WDT=6,ESP_RST_WDT=7} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
