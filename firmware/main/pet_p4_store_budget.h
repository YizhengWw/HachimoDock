/* Profiled ordinary-save budget, including worst JSON escaping and print slack.
 * Pure admission only; no heap/global scratch/IO. Keep Python/Rust/JS in parity. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static inline size_t pet_store_budget_begin(bool page,bool alarms){return 128U+(page?64U:0U)+(alarms?268U:0U);}
static inline size_t pet_store_budget_field(const char *name,bool string,unsigned bytes){return strlen(name)+4U+(string?2U+6U*bytes:11U);}
static inline bool pet_store_budget_fits(size_t bytes,bool v3){return bytes<=(v3?2303U:1023U);}
