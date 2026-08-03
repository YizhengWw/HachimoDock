#ifndef BOARD_RUNTIME_USB_SERIAL_H
#define BOARD_RUNTIME_USB_SERIAL_H

#include <stdbool.h>
#include <stddef.h>

/* Callback signature matches br_mqtt_publish_callback for unified dispatch */
typedef void (*br_serial_message_callback)(const char *topic, const char *payload, void *userdata);

typedef struct {
  int fd;
  bool connected;
  bool peer_acked;
  char device_path[256];
  int baud_rate;
  char read_buffer[131072]; /* 128KB — large enough for base64 asset chunks */
  size_t read_used;
  long long last_hello_ms;
  br_serial_message_callback on_message;
  void *userdata;
} br_usb_serial;

/* Detect whether USB gadget is connected to a host.
 * Checks UDC state sysfs nodes for "configured" status. */
bool br_usb_gadget_connected(void);

/* Open serial port (e.g. /dev/ttyGS0), configure 115200 8N1 raw mode.
 * Returns 0 on success, -1 on failure. */
int br_usb_serial_open(br_usb_serial *serial, const char *path, int baud_rate,
                       br_serial_message_callback on_message, void *userdata);

/* Close serial port */
void br_usb_serial_close(br_usb_serial *serial);

/* Non-blocking poll: read available data, parse complete JSON lines,
 * invoke on_message for each. Returns 0 normally, -1 on disconnect. */
int br_usb_serial_poll(br_usb_serial *serial);

/* Send a message as JSON line: {"topic":"...","payload":...}\n
 * Returns 0 on success, -1 on failure. */
int br_usb_serial_send(br_usb_serial *serial, const char *topic, const char *payload);

/* Send hello handshake message (call every 3s until peer_acked) */
int br_usb_serial_send_hello(br_usb_serial *serial, const char *board_device_id);

/* Send a pre-formatted JSON string as a line (appends \n) */
int br_usb_serial_send_raw(br_usb_serial *serial, const char *json);

#endif
