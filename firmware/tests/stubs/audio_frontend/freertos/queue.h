#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t *data; size_t capacity, size, head, count; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(size_t capacity, size_t size, uint8_t *data, StaticQueue_t *control);
int xQueueReset(QueueHandle_t queue);
int xQueueSend(QueueHandle_t queue, const void *item, int ticks);
int xQueueReceive(QueueHandle_t queue, void *item, int ticks);
void vQueueDelete(QueueHandle_t queue);
