#pragma once
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
extern _Thread_local unsigned hint_test_lock_depth;
static inline void hint_enter(portMUX_TYPE *m){assert(!hint_test_lock_depth);assert(!pthread_mutex_lock(m));hint_test_lock_depth=1;}
static inline void hint_leave(portMUX_TYPE *m){assert(hint_test_lock_depth==1);hint_test_lock_depth=0;assert(!pthread_mutex_unlock(m));}
#define portENTER_CRITICAL(m) hint_enter(m)
#define portEXIT_CRITICAL(m) hint_leave(m)
