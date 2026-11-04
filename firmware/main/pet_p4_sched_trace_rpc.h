/* Separately compiled, passive scheduler evidence. No ordinary capability or
 * command is advertised. Numeric records only: no names, addresses or paths. */
#pragma once
#include <stdbool.h>
#include "pet_p4_sched_trace.h"
typedef void (*pet_sched_probe_send_fn)(const char *line,void *context);
#if PET_P4_SCHED_TRACE_ENABLE
bool pet_p4_sched_trace_rpc_handle(const char *line,pet_sched_probe_send_fn send,void *context);
#else
static inline bool pet_p4_sched_trace_rpc_handle(const char *line,pet_sched_probe_send_fn send,void *context){
  (void)line;(void)send;(void)context;return false;
}
#endif
