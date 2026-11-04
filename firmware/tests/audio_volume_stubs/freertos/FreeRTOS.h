#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) ((void)pthread_mutex_lock(m))
#define portEXIT_CRITICAL(m) ((void)pthread_mutex_unlock(m))
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(n) (n)
#define portTICK_PERIOD_MS 1
#define configASSERT assert
