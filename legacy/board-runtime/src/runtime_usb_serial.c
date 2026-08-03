#include "runtime_usb_serial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "runtime_common.h"
#include "runtime_json.h"

/* --- USB gadget detection --- */

bool br_usb_gadget_connected(void) {
  DIR *dir;
  struct dirent *entry;
  char path[256];
  char state[64];

  dir = opendir("/sys/class/udc");
  if (!dir) {
    return false;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    snprintf(path, sizeof(path), "/sys/class/udc/%s/state", entry->d_name);
    if (br_read_text_file(path, state, sizeof(state))) {
      char *trimmed = br_trim(state);
      if (strcmp(trimmed, "configured") == 0) {
        closedir(dir);
        return true;
      }
    }
  }

  closedir(dir);
  return false;
}

/* --- Serial port --- */

static speed_t br_baud_to_speed(int baud_rate) {
  switch (baud_rate) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return B115200;
  }
}

int br_usb_serial_open(br_usb_serial *serial, const char *path, int baud_rate,
                       br_serial_message_callback on_message, void *userdata) {
  struct termios tty;
  int fd;

  if (!serial || !path) {
    return -1;
  }

  memset(serial, 0, sizeof(*serial));
  serial->fd = -1;

  fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "usb_serial: cannot open %s: %s\n", path, strerror(errno));
    return -1;
  }

  if (tcgetattr(fd, &tty) != 0) {
    fprintf(stderr, "usb_serial: tcgetattr failed: %s\n", strerror(errno));
    close(fd);
    return -1;
  }

  /* Raw mode: no echo, no signals, no canonical processing */
  cfmakeraw(&tty);

  speed_t speed = br_baud_to_speed(baud_rate);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);

  /* 8N1 */
  tty.c_cflag &= ~(unsigned int) PARENB;
  tty.c_cflag &= ~(unsigned int) CSTOPB;
  tty.c_cflag &= ~(unsigned int) CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cflag |= CLOCAL | CREAD;

  /* Non-blocking reads: return immediately */
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    fprintf(stderr, "usb_serial: tcsetattr failed: %s\n", strerror(errno));
    close(fd);
    return -1;
  }

  tcflush(fd, TCIOFLUSH);

  snprintf(serial->device_path, sizeof(serial->device_path), "%s", path);
  serial->baud_rate = baud_rate;
  serial->fd = fd;
  serial->connected = true;
  serial->peer_acked = false;
  serial->read_used = 0;
  serial->last_hello_ms = 0;
  serial->on_message = on_message;
  serial->userdata = userdata;

  fprintf(stderr, "usb_serial: opened %s at %d baud\n", path, baud_rate);
  return 0;
}

void br_usb_serial_close(br_usb_serial *serial) {
  if (!serial) {
    return;
  }
  if (serial->fd >= 0) {
    close(serial->fd);
  }
  serial->fd = -1;
  serial->connected = false;
  serial->peer_acked = false;
}

/* Process a single complete JSON line */
static void br_usb_serial_process_line(br_usb_serial *serial, char *line, size_t length) {
  br_json_token tokens[64];
  int count;
  int topic_idx, payload_idx;
  char topic[256];

  if (length == 0) {
    return;
  }

  count = br_json_parse(line, length, tokens, 64);
  if (count < 1 || tokens[0].type != BR_JSON_OBJECT) {
    fprintf(stderr, "usb_serial: invalid JSON line\n");
    return;
  }

  topic_idx = br_json_find_key(line, tokens, count, 0, "topic");
  if (topic_idx < 0 || !br_json_token_to_string(line, &tokens[topic_idx], topic, sizeof(topic))) {
    fprintf(stderr, "usb_serial: missing 'topic' field\n");
    return;
  }

  payload_idx = br_json_find_key(line, tokens, count, 0, "payload");
  if (payload_idx < 0) {
    fprintf(stderr, "usb_serial: missing 'payload' field\n");
    return;
  }

  /* Extract raw payload JSON substring */
  char payload[BR_MAX_JSON];
  if (!br_json_copy_raw(line, &tokens[payload_idx], payload, sizeof(payload))) {
    fprintf(stderr, "usb_serial: payload too large\n");
    return;
  }

  if (serial->on_message) {
    serial->on_message(topic, payload, serial->userdata);
  }
}

int br_usb_serial_poll(br_usb_serial *serial) {
  ssize_t nread;
  size_t remaining;

  if (!serial || serial->fd < 0 || !serial->connected) {
    return -1;
  }

  /* Read all available data in a loop until EAGAIN */
  for (;;) {
    remaining = sizeof(serial->read_buffer) - serial->read_used - 1;
    if (remaining == 0) {
      /* Buffer full without a newline -- discard */
      fprintf(stderr, "usb_serial: read buffer overflow, discarding\n");
      serial->read_used = 0;
      remaining = sizeof(serial->read_buffer) - 1;
    }

    nread = read(serial->fd, serial->read_buffer + serial->read_used, remaining);
    if (nread < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        break; /* No more data available right now */
      }
      if (errno == EIO) {
        /* CDC-ACM gadgets can report EIO while the host has not opened the
           serial endpoint yet. Keep the fd alive so a later host reconnect can
           continue without respawning the whole runtime. */
        break;
      }
      fprintf(stderr, "usb_serial: read error: %s\n", strerror(errno));
      br_usb_serial_close(serial);
      return -1;
    }
    if (nread == 0) {
      /* CDC-ACM: returns 0 when host not connected or no data.
         Don't spin — just return and let select() handle timing. */
      break;
    }

    serial->read_used += (size_t) nread;
  }

  /* If no data was read at all, avoid busy-loop on persistent EOF from gadget */
  if (serial->read_used == 0) {
    return 0;
  }

  /* Process all complete lines */
  size_t scan_start = 0;
  for (size_t i = 0; i < serial->read_used; i++) {
    if (serial->read_buffer[i] == '\n') {
      serial->read_buffer[i] = '\0';
      size_t line_len = i - scan_start;
      if (line_len > 0) {
        br_usb_serial_process_line(serial, serial->read_buffer + scan_start, line_len);
      }
      scan_start = i + 1;
    }
  }

  /* Move remaining partial line to front */
  if (scan_start > 0) {
    size_t leftover = serial->read_used - scan_start;
    if (leftover > 0) {
      memmove(serial->read_buffer, serial->read_buffer + scan_start, leftover);
    }
    serial->read_used = leftover;
  }

  return 0;
}

static int br_usb_serial_write_all(int fd, const char *data, size_t length) {
  int eagain_count = 0;
  while (length > 0) {
    ssize_t written = write(fd, data, length);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        eagain_count++;
        if (eagain_count > 500) {
          /* 500ms total wait — USB host not consuming data, give up */
          fprintf(stderr, "usb_serial: write timeout after %d EAGAIN retries\n", eagain_count);
          errno = ETIMEDOUT;
          return -1;
        }
        br_sleep_ms(1);
        continue;
      }
      return -1;
    }
    /* Partial write succeeded — reset EAGAIN counter */
    eagain_count = 0;
    data += (size_t) written;
    length -= (size_t) written;
  }
  return 0;
}

int br_usb_serial_send(br_usb_serial *serial, const char *topic, const char *payload) {
  char line[BR_MAX_JSON + 512];
  int len;

  if (!serial || serial->fd < 0 || !serial->connected || !topic || !payload) {
    return -1;
  }

  len = snprintf(line, sizeof(line), "{\"topic\":\"%s\",\"payload\":%s}\n", topic, payload);
  if (len < 0 || (size_t) len >= sizeof(line)) {
    fprintf(stderr, "usb_serial: send message too large\n");
    return -1;
  }

  if (br_usb_serial_write_all(serial->fd, line, (size_t) len) != 0) {
    if (errno == ETIMEDOUT || errno == EIO) {
      /* Host may be detached or not consuming data yet. Drop this heartbeat or
         ack, but keep the gadget fd open so future host opens can recover. */
      fprintf(stderr, "usb_serial: send unavailable for topic '%s' (dropped: %s)\n",
              topic,
              errno == ETIMEDOUT ? "timeout" : "io");
      return -1;
    }
    fprintf(stderr, "usb_serial: write error: %s\n", strerror(errno));
    br_usb_serial_close(serial);
    return -1;
  }

  return 0;
}

int br_usb_serial_send_hello(br_usb_serial *serial, const char *board_device_id) {
  char payload[BR_MAX_JSON];
  char ts[64];
  size_t used = 0;

  if (!serial || !board_device_id) {
    return -1;
  }

  br_iso8601_now(ts, sizeof(ts));

  br_snprintf_append(payload, sizeof(payload), &used,
    "{\"online\":true,\"boardDeviceId\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, board_device_id);
  br_snprintf_append(payload, sizeof(payload), &used,
    "\",\"transport\":\"usb\",\"ts\":\"");
  br_json_escape_append(payload, sizeof(payload), &used, ts);
  br_snprintf_append(payload, sizeof(payload), &used,
    "\",\"tsMs\":%lld}", br_now_ms());

  return br_usb_serial_send(serial, "hello", payload);
}

int br_usb_serial_send_raw(br_usb_serial *serial, const char *json) {
  /* Atomic JSON-line write. Previously this split the payload into
     `write_all(json)` + `write_all("\n")`, which on a host-stalled gadget
     would EAGAIN-timeout between the two calls: the JSON body landed in the
     ring buffer but the newline was dropped, and the next packet's JSON got
     concatenated onto the unterminated line — host BufReader saw garbage
     ("[usb_serial] invalid JSON: invalid number at line 1 column 2"). All
     ack types (asset/ack via send_ack_ex, widget-install-ack via send) were
     affected because the host parser fails the whole line and skips it,
     which in turn timed out the host's commit waiters → ComponentCenter
     installs and appearance OTA both reported "未收到板端确认".

     Fix: copy json + '\n' into one stack buffer and write_all once. The
     EAGAIN/timeout retry inside write_all still applies, but it cannot
     drop just the trailing newline anymore. */
  static char line[BR_MAX_JSON + 2];
  size_t json_len;
  if (!serial || serial->fd < 0 || !serial->connected || !json) return -1;
  json_len = strlen(json);
  if (json_len + 1 > sizeof(line)) {
    fprintf(stderr, "usb_serial: raw send too large (%zu bytes)\n", json_len);
    return -1;
  }
  memcpy(line, json, json_len);
  line[json_len] = '\n';
  if (br_usb_serial_write_all(serial->fd, line, json_len + 1) != 0) {
    if (errno == ETIMEDOUT || errno == EIO) {
      /* Host detached or not draining — drop message but keep gadget fd open */
      fprintf(stderr, "usb_serial: raw send unavailable (dropped: %s)\n",
              errno == ETIMEDOUT ? "timeout" : "io");
      return -1;
    }
    fprintf(stderr, "usb_serial: raw write error: %s\n", strerror(errno));
    br_usb_serial_close(serial);
    return -1;
  }
  return 0;
}
