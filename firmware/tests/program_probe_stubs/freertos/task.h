#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
