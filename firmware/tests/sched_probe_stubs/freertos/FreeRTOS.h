#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define configNUMBER_OF_CORES 2
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
extern _Thread_local unsigned probe_test_core,probe_test_irq,probe_test_lock_depth;
static inline UBaseType_t probe_mask(void){unsigned old=probe_test_irq;probe_test_irq++;return old;}
static inline void probe_unmask(UBaseType_t old){assert(probe_test_irq>old);probe_test_irq=old;}
static inline void probe_enter(portMUX_TYPE *lock){assert(!probe_test_lock_depth);assert(!pthread_mutex_lock(lock));probe_test_lock_depth=1;}
static inline void probe_leave(portMUX_TYPE *lock){assert(probe_test_lock_depth==1);probe_test_lock_depth=0;assert(!pthread_mutex_unlock(lock));}
#define portSET_INTERRUPT_MASK_FROM_ISR() probe_mask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(old) probe_unmask(old)
#define portGET_CORE_ID() probe_test_core
#define portENTER_CRITICAL(m) probe_enter(m)
#define portEXIT_CRITICAL(m) probe_leave(m)
#define portENTER_CRITICAL_ISR(m) probe_enter(m)
#define portEXIT_CRITICAL_ISR(m) probe_leave(m)
