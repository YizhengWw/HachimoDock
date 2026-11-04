#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, TickType_t ticks);
int xSemaphoreGiveRecursive(SemaphoreHandle_t lock);
