#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define configNUMBER_OF_CORES 2
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
extern _Thread_local unsigned sched_test_core,sched_test_irq,sched_test_trace_depth;
static inline UBaseType_t sched_test_mask(void){unsigned old=sched_test_irq;sched_test_irq++;return old;}
static inline void sched_test_unmask(UBaseType_t old){assert(sched_test_irq>old);sched_test_irq=old;}
static inline void sched_test_enter(portMUX_TYPE *lock){
    assert(!sched_test_trace_depth);assert(!pthread_mutex_lock(lock));sched_test_trace_depth=1;
}
static inline void sched_test_leave(portMUX_TYPE *lock){
    assert(sched_test_trace_depth==1);sched_test_trace_depth=0;assert(!pthread_mutex_unlock(lock));
}
#define portSET_INTERRUPT_MASK_FROM_ISR() sched_test_mask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(old) sched_test_unmask(old)
#define portGET_CORE_ID() sched_test_core
#define portENTER_CRITICAL_ISR(m) sched_test_enter(m)
#define portEXIT_CRITICAL_ISR(m) sched_test_leave(m)
