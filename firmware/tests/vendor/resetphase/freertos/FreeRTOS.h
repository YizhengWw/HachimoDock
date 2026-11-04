#pragma once
typedef void *TaskHandle_t;
int xPortInIsrContext(void);
int xPortCanYield(void);
unsigned xPortGetCoreID(void);
