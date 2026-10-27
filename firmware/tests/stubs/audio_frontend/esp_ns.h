#pragma once
#include <stdint.h>
typedef void *ns_handle_t;
ns_handle_t ns_create(int frame_ms);
void ns_destroy(ns_handle_t handle);
void ns_process(ns_handle_t handle, int16_t *input, int16_t *output);
