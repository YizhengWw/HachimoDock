#pragma once
#include <stdint.h>
uint32_t resetphase_test_mstatus(void);
#define MSTATUS_MIE (1U<<3)
#define RV_READ_CSR(name) resetphase_test_mstatus()
