#pragma once
#include <assert.h>
typedef unsigned portMUX_TYPE;
extern unsigned io_meta_depth;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) do{(void)(m);assert(!io_meta_depth);io_meta_depth++;}while(0)
#define portEXIT_CRITICAL(m) do{(void)(m);assert(io_meta_depth==1);io_meta_depth--;}while(0)
