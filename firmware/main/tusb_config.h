#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_FREERTOS
#endif

#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUD_ENABLED 1
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUSB_DEBUG 0

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_EPSIZE 512
#define CFG_TUD_VENDOR_RX_BUFSIZE (512 * 16)
#define CFG_TUD_VENDOR_TX_BUFSIZE (512 * 8)

#ifdef __cplusplus
}
#endif
