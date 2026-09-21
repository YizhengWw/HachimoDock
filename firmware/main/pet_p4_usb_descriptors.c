#include <stdint.h>
#include <string.h>

#include "tusb.h"

#define PET_P4_USB_VID 0x303A
#define PET_P4_USB_PID 0x4040
#define PET_P4_MS_OS_20_VENDOR_CODE 0x20

enum {
  ITF_NUM_VENDOR = 0,
  ITF_NUM_TOTAL,
};

enum {
  EPNUM_VENDOR_OUT = 0x01,
  EPNUM_VENDOR_IN = 0x81,
};

static tusb_desc_device_t const desc_device = {
  .bLength = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = 0x0210,
  .bDeviceClass = TUSB_CLASS_VENDOR_SPECIFIC,
  .bDeviceSubClass = 0,
  .bDeviceProtocol = 0,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor = PET_P4_USB_VID,
  .idProduct = PET_P4_USB_PID,
  .bcdDevice = 0x0100,
  .iManufacturer = 0x01,
  .iProduct = 0x02,
  .iSerialNumber = 0x03,
  .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *) &desc_device;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static uint8_t const desc_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 0x04, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, CFG_TUD_VENDOR_EPSIZE),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_configuration;
}

#define PET_P4_BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define PET_P4_MS_OS_20_DESC_LEN 0x2E

static uint8_t const desc_bos[] = {
  TUD_BOS_DESCRIPTOR(PET_P4_BOS_TOTAL_LEN, 1),
  TUD_BOS_MS_OS_20_DESCRIPTOR(PET_P4_MS_OS_20_DESC_LEN, PET_P4_MS_OS_20_VENDOR_CODE),
};

uint8_t const *tud_descriptor_bos_cb(void) {
  return desc_bos;
}

static uint8_t const desc_ms_os_20[] = {
  U16_TO_U8S_LE(0x000A),
  U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
  U32_TO_U8S_LE(0x06030000),
  U16_TO_U8S_LE(PET_P4_MS_OS_20_DESC_LEN),

  U16_TO_U8S_LE(0x0008),
  U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
  0x00,
  0x00,
  U16_TO_U8S_LE(PET_P4_MS_OS_20_DESC_LEN - 0x0A),

  U16_TO_U8S_LE(0x0008),
  U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
  ITF_NUM_VENDOR,
  0x00,
  U16_TO_U8S_LE(PET_P4_MS_OS_20_DESC_LEN - 0x0A - 0x08),

  U16_TO_U8S_LE(0x0014),
  U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
  'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(
  sizeof(desc_ms_os_20) == PET_P4_MS_OS_20_DESC_LEN,
  "incorrect Microsoft OS 2.0 descriptor size"
);

bool tud_vendor_control_xfer_cb(
    uint8_t rhport,
    uint8_t stage,
    tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
      request->bRequest != PET_P4_MS_OS_20_VENDOR_CODE ||
      request->wIndex != 7) {
    return false;
  }
  return tud_control_xfer(
    rhport,
    request,
    (void *)(uintptr_t) desc_ms_os_20,
    PET_P4_MS_OS_20_DESC_LEN
  );
}

static char const *const string_desc_arr[] = {
  (const char[]) {0x09, 0x04},
  "OpenClaw",
  "OpenClaw P4 Native USB",
  "p4-native-001",
  "P4 vendor bulk",
};

static uint16_t desc_str[64];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  uint8_t chr_count;
  if (index == 0) {
    memcpy(&desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
    const char *str = string_desc_arr[index];
    chr_count = (uint8_t) strlen(str);
    if (chr_count > 63) chr_count = 63;
    for (uint8_t i = 0; i < chr_count; i += 1) {
      desc_str[1 + i] = (uint8_t) str[i];
    }
  }
  desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return desc_str;
}
