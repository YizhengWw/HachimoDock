#pragma once
#include "pet_p4_storage_io_trace.h"
typedef void (*pet_storage_io_send_fn)(const char*,void*);
#if PET_P4_STORAGE_IO_TRACE_ENABLE
bool pet_storage_io_rpc_handle(const char *line,pet_storage_io_send_fn send,void *context);
#else
static inline bool pet_storage_io_rpc_handle(const char *s,pet_storage_io_send_fn f,void *c){(void)s;(void)f;(void)c;return false;}
#endif
