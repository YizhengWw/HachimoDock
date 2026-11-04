#pragma once
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) ((void)pthread_mutex_lock(m))
#define portEXIT_CRITICAL(m) ((void)pthread_mutex_unlock(m))
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
#ifdef PROBE_STUB_WORD_STACK
typedef uint32_t StackType_t;
#else
typedef uint8_t StackType_t;
#endif
