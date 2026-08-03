#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "runtime_common.h"
#include "runtime_mqtt.h"
#include "runtime_protocol.h"
#include "screen_page.h"
#include "touch_gesture.h"

#ifdef __linux__
#include <linux/input.h>
#define BR_EVENT_SIZE_DEFAULT ((int) sizeof(struct input_event))
#else
#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define BTN_TOUCH 330
#define ABS_X 0
#define ABS_Y 1
#define ABS_MT_POSITION_X 53
#define ABS_MT_POSITION_Y 54
#define BR_EVENT_SIZE_DEFAULT 16
#endif

typedef struct {
  char root_dir[BR_MAX_PATH];
  char mqtt_url[256];
  char mqtt_username[128];
  char mqtt_password[128];
  char board_device_id[128];
  char input_action_topic[BR_MAX_TOPIC];
  char touch_request_path[BR_MAX_PATH];
  char screen_page_path[BR_MAX_PATH];
  char usb_touch_action_path[BR_MAX_PATH];
  char button_config_path[BR_MAX_PATH];
  /* widget-events bridge: same pattern as board_rotary_input.c — when a widget
     is active AND screen-page is stats, emit screen.region.tap to
     .widget-events instead of falling through to the default page-toggle */
  char active_widget_path[BR_MAX_PATH];
  char widget_events_path[BR_MAX_PATH];
  char touch_device[BR_MAX_PATH];
  char system_reset_cmd[256];
  int event_size;
  int swipe_threshold;
  int long_press_ms;
  bool usb_mode;
} br_touch_config;

static void br_touch_logf(const char *format, ...) {
  va_list args;
  fprintf(stdout, "[touch-input] ");
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

static void br_touch_load_config(br_touch_config *config, const char *root_dir) {
  br_normalize_text(root_dir, ".", config->root_dir, sizeof(config->root_dir));
  br_normalize_text(getenv("PET_CLAW_MQTT_URL") ? getenv("PET_CLAW_MQTT_URL") : getenv("MQTT_URL"),
                    "mqtt://broker.openclaw.example:1883",
                    config->mqtt_url,
                    sizeof(config->mqtt_url));
  br_normalize_text(getenv("PET_CLAW_MQTT_USERNAME") ? getenv("PET_CLAW_MQTT_USERNAME") : getenv("MQTT_USERNAME"),
                    "device",
                    config->mqtt_username,
                    sizeof(config->mqtt_username));
  br_normalize_text(getenv("PET_CLAW_MQTT_PASSWORD") ? getenv("PET_CLAW_MQTT_PASSWORD") : getenv("MQTT_PASSWORD"),
                    "",
                    config->mqtt_password,
                    sizeof(config->mqtt_password));
  br_normalize_topic_part(getenv("PET_CLAW_DEVICE_ID"), "linux-pet-01", config->board_device_id, sizeof(config->board_device_id));
  snprintf(config->input_action_topic, sizeof(config->input_action_topic), "claw-pet/board/%s/input/action", config->board_device_id);
  br_normalize_text(getenv("PET_CLAW_TOUCH_DEVICE") ? getenv("PET_CLAW_TOUCH_DEVICE") : getenv("PET_SCREEN_TOUCH_DEVICE"),
                    "",
                    config->touch_device,
                    sizeof(config->touch_device));
  snprintf(config->touch_request_path, sizeof(config->touch_request_path), "%s/.touch-request", config->root_dir);
  snprintf(config->screen_page_path, sizeof(config->screen_page_path), "%s/.screen-page", config->root_dir);
  snprintf(config->usb_touch_action_path, sizeof(config->usb_touch_action_path), "%s/.usb-touch-action", config->root_dir);
  snprintf(config->button_config_path, sizeof(config->button_config_path), "%s/.button-config", config->root_dir);
  snprintf(config->active_widget_path, sizeof(config->active_widget_path), "%s/.active-widget", config->root_dir);
  snprintf(config->widget_events_path, sizeof(config->widget_events_path), "%s/.widget-events", config->root_dir);
  br_normalize_text(getenv("PET_TOUCH_SYSTEM_RESET_CMD"),
                    "systemctl restart board-runtime || /etc/init.d/board-runtime restart",
                    config->system_reset_cmd,
                    sizeof(config->system_reset_cmd));
  {
    const char *transport = getenv("BOARD_TRANSPORT");
    config->usb_mode = (transport && strcmp(transport, "usb") == 0);
  }
  config->event_size = getenv("PET_SCREEN_INPUT_EVENT_SIZE") ? atoi(getenv("PET_SCREEN_INPUT_EVENT_SIZE")) : BR_EVENT_SIZE_DEFAULT;
  if (config->event_size <= 0) {
    config->event_size = BR_EVENT_SIZE_DEFAULT;
  }
  config->swipe_threshold = getenv("PET_SCREEN_SWIPE_THRESHOLD") ? atoi(getenv("PET_SCREEN_SWIPE_THRESHOLD")) : 40;
  config->long_press_ms = getenv("PET_SCREEN_LONG_PRESS_MS") ? atoi(getenv("PET_SCREEN_LONG_PRESS_MS")) : 5000;
}

static bool br_touch_file_exists(const char *path) {
  struct stat st;
  return path && *path && stat(path, &st) == 0;
}

/* True iff .active-widget is non-empty AND .screen-page == "stats". Used to
   gate touch-tap → .widget-events forwarding so the widget owns taps only
   while it's visible on screen (no surprise dead clicks on the dog page). */
static bool br_widget_active_on_stats(const br_touch_config *config) {
  char buf[128];
  if (!br_read_text_file(config->active_widget_path, buf, sizeof(buf))) {
    return false;
  }
  size_t n = strlen(buf);
  while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t')) {
    buf[--n] = '\0';
  }
  if (n == 0) return false;
  char page[32];
  if (!br_read_text_file(config->screen_page_path, page, sizeof(page))) {
    return false;
  }
  size_t pn = strlen(page);
  while (pn > 0 && (page[pn-1] == '\n' || page[pn-1] == '\r' || page[pn-1] == ' ')) {
    page[--pn] = '\0';
  }
  return strcmp(page, "stats") == 0;
}

static bool br_touch_read_button_action(
  const br_touch_config *config,
  const char *event,
  char *action,
  size_t action_size
) {
  char json[4096];
  if (!config || !event || !action || action_size == 0) {
    return false;
  }
  action[0] = '\0';
  if (!br_read_text_file(config->button_config_path, json, sizeof(json))) {
    return false;
  }
  return br_button_config_find_action_json(json, event, action, action_size);
}

static const char *br_touch_widget_event_for_action(const char *action) {
  if (!action) {
    return NULL;
  }
  if (strcmp(action, "negative_screen_primary") == 0) {
    return "screen.region.tap";
  }
  if (strcmp(action, "negative_screen_secondary") == 0) {
    return "screen.region.long_press";
  }
  if (strcmp(action, "negative_screen_adjust") == 0) {
    return "knob.rotate_cw / knob.rotate_ccw";
  }
  return NULL;
}

static bool br_touch_append_widget_event(
  const br_touch_config *config,
  const char *event
) {
  FILE *f;
  if (!config || !event) {
    return false;
  }
  if (!br_widget_active_on_stats(config)) {
    br_touch_logf("widget event skipped: no active stats widget (%s)", event);
    return true;
  }
  f = fopen(config->widget_events_path, "a");
  if (!f) {
    br_touch_logf("widget event open failed: %s", config->widget_events_path);
    return true;
  }
  fprintf(f, "{\"control\":\"屏幕区域\",\"event\":\"%s\",\"ts\":%lld}\n",
          event,
          br_now_ms());
  fclose(f);
  br_touch_logf("→ widget-events: 屏幕区域 %s", event);
  return true;
}

static void br_touch_toggle_screen_page(const br_touch_config *config, const char *reason) {
  char current_page[32];
  char next_page[32];
  if (!br_read_text_file(config->screen_page_path, current_page, sizeof(current_page))) {
    current_page[0] = '\0';
  }
  if (br_screen_page_toggle_main_stats(current_page, next_page, sizeof(next_page)) &&
      br_atomic_write_text(config->screen_page_path, next_page)) {
    br_touch_logf("screen-page=%s (%s from=%s)",
                  next_page,
                  reason ? reason : "button_config",
                  current_page[0] ? current_page : "empty");
  } else {
    br_touch_logf("screen-page write failed: %s", config->screen_page_path);
  }
}

static bool br_touch_apply_button_config_action(
  const br_touch_config *config,
  const char *event
) {
  char action[64];
  int exit_code;
  if (!br_touch_read_button_action(config, event, action, sizeof(action))) {
    return false;
  }
  if (strcmp(action, "disabled") == 0 || strcmp(action, "voice_ptt") == 0) {
    br_touch_logf("touch action %s -> %s", event, action);
    return true;
  }
  if (strcmp(action, "system_page") == 0) {
    br_touch_toggle_screen_page(config, event);
    return true;
  }
  if (strcmp(action, "system_reset") == 0) {
    br_touch_logf("system reset command: %s", config->system_reset_cmd);
    exit_code = system(config->system_reset_cmd);
    if (exit_code != 0) {
      br_touch_logf("system reset command exited with %d", exit_code);
    }
    return true;
  }
  if (br_touch_widget_event_for_action(action) != NULL) {
    /* Routed to the negative-screen widget: emit the NATIVE event for the
       actual touch gesture (tap→screen.region.tap, long_press→
       screen.region.long_press), regardless of which primary/secondary slot
       the desktop assigned, so buttons.json matches and tap/long_press never
       cross-wire. */
    return br_touch_append_widget_event(config, event);
  }
  return false;
}

static bool br_detect_touch_device(br_touch_config *config) {
  if (br_touch_file_exists(config->touch_device)) {
    return true;
  }
  if (br_touch_file_exists("/dev/input/touchscreen")) {
    br_normalize_text("/dev/input/touchscreen", "", config->touch_device, sizeof(config->touch_device));
    return true;
  }
#ifdef __linux__
  FILE *file = fopen("/proc/bus/input/devices", "rb");
  if (file) {
    char line[512];
    char block[2048] = "";
    while (fgets(line, sizeof(line), file)) {
      if (strcmp(line, "\n") == 0) {
        if ((strstr(block, "touch") || strstr(block, "Touch") || strstr(block, "rtp") || strstr(block, "ts")) &&
            strstr(block, "event")) {
          char *event = strstr(block, "event");
          if (event) {
            char candidate[BR_MAX_PATH];
            char event_name[32];
            size_t index = 0;
            while (event[index] && !isspace((unsigned char) event[index]) && index < sizeof(event_name) - 1) {
              event_name[index] = event[index];
              index += 1;
            }
            event_name[index] = '\0';
            snprintf(candidate, sizeof(candidate), "/dev/input/%s", event_name);
            if (br_touch_file_exists(candidate)) {
              br_normalize_text(candidate, "", config->touch_device, sizeof(config->touch_device));
              fclose(file);
              return true;
            }
          }
        }
        block[0] = '\0';
        continue;
      }
      if (strlen(block) + strlen(line) + 1 < sizeof(block)) {
        strcat(block, line);
      }
    }
    fclose(file);
  }
#endif
  if (br_touch_file_exists("/dev/input/event0")) {
    br_normalize_text("/dev/input/event0", "", config->touch_device, sizeof(config->touch_device));
    return true;
  }
  return false;
}

static void br_emit_touch_action(
  br_mqtt_client *mqtt,
  const br_touch_config *config,
  const br_touch_action *action
) {
  br_input_action input;
  char payload[BR_MAX_JSON];
  memset(&input, 0, sizeof(input));
  br_normalize_text(br_touch_action_type_name(action->type), "", input.type, sizeof(input.type));
  input.has_duration_ms = true;
  input.duration_ms = action->duration_ms;
  if (action->type == BR_TOUCH_TAP || action->type == BR_TOUCH_LONG_PRESS) {
    input.has_x = true;
    input.has_y = true;
    input.x = action->x;
    input.y = action->y;
  } else {
    input.has_dx = true;
    input.has_dy = true;
    input.dx = action->dx;
    input.dy = action->dy;
  }

  if (br_build_input_action_payload(config->board_device_id,
                                    config->board_device_id,
                                    "board-runtime",
                                    &input,
                                    br_now_ms(),
                                    payload,
                                    sizeof(payload)) != 0) {
    return;
  }
  if (config->usb_mode) {
    /* USB mode: write action to file for board-server to forward via serial */
    br_atomic_write_text(config->usb_touch_action_path, payload);
    br_touch_logf("action %s (usb file)", input.type);
  } else if (br_mqtt_client_publish(mqtt, config->input_action_topic, payload, false) == 0) {
    br_touch_logf("action %s", input.type);
  }

  /* Local touch feedback: request a screen-local touch insert without
     overwriting the canonical business state. */
  {
    char marker[128];
    snprintf(marker, sizeof(marker), "%lld %s\n", br_now_ms(), input.type);
    br_atomic_write_text(config->touch_request_path, marker);
    br_touch_logf("local touch request=%s", input.type);
  }

  /* Widget bridge: when a widget is active and we're on stats page, tap
     becomes the widget event screen.region.tap (consumed by board-widget-runtime
     for widget.json transitions). Swipes keep flowing through to the page
     toggle below — they're the system-wide escape gesture and must NOT be
     captured by the widget. */
  if (!strcmp(input.type, "tap") || !strcmp(input.type, "long_press")) {
    const char *evt = !strcmp(input.type, "tap")
        ? "screen.region.tap" : "screen.region.long_press";
    if (br_touch_apply_button_config_action(config, evt)) {
      return;
    }
    if (br_widget_active_on_stats(config) && br_touch_append_widget_event(config, evt)) {
      /* don't fall through to page-toggle — tap is widget event, not navigation */
      return;
    }
  }

  /* Two-page carousel: any swipe toggles between main and stats.  On the
     rotated XPT2046 panel, a physical horizontal swipe may arrive as up/down. */
  if (br_screen_page_touch_action_should_toggle(input.type)) {
    br_touch_toggle_screen_page(config, input.type);
  }
}

static bool br_parse_input_event_packet(
  const unsigned char *packet,
  int event_size,
  unsigned short *type,
  unsigned short *code,
  int *value
) {
  if (!packet || !type || !code || !value) {
    return false;
  }
  if (event_size == 24) {
    memcpy(type, packet + 16, sizeof(*type));
    memcpy(code, packet + 18, sizeof(*code));
    memcpy(value, packet + 20, sizeof(*value));
    return true;
  }
  if (event_size == 16) {
    memcpy(type, packet + 8, sizeof(*type));
    memcpy(code, packet + 10, sizeof(*code));
    memcpy(value, packet + 12, sizeof(*value));
    return true;
  }
  return false;
}

#ifdef __linux__
static void br_handle_input_event(
  br_touch_gesture_state *gesture,
  br_mqtt_client *mqtt,
  const br_touch_config *config,
  unsigned short type,
  unsigned short code,
  int value
) {
  br_touch_action action;
  long long now_ms = br_now_ms();

  if (type == EV_ABS) {
    if (code == ABS_X || code == ABS_MT_POSITION_X) {
      br_touch_gesture_set_position(gesture, value, gesture->current_y);
    } else if (code == ABS_Y || code == ABS_MT_POSITION_Y) {
      br_touch_gesture_set_position(gesture, gesture->current_x, value);
    }
    return;
  }

  if (type == EV_KEY && code == BTN_TOUCH) {
    if (value == 1 && !gesture->touch_down) {
      br_touch_gesture_start(gesture, now_ms);
    } else if (value == 0 && br_touch_gesture_finish(gesture, now_ms, &action)) {
      br_emit_touch_action(mqtt, config, &action);
    }
    return;
  }

  if (type == EV_SYN && br_touch_gesture_sync(gesture, now_ms, &action)) {
    br_emit_touch_action(mqtt, config, &action);
  }
}
#endif

int main(int argc, char **argv) {
  br_touch_config config;
  br_mqtt_client mqtt;
  char client_id[128];
  int input_fd = -1;
  br_touch_gesture_state gesture;

  if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    printf("usage: %s [runtime-root]\n", argv[0]);
    return 0;
  }

  br_touch_load_config(&config, argc > 1 ? argv[1] : ".");
  if (!br_detect_touch_device(&config)) {
    br_touch_logf("no touch device found");
    return 1;
  }

  if (!config.usb_mode) {
    snprintf(client_id, sizeof(client_id), "board-touch-%s-%d", config.board_device_id, (int) getpid());
    br_mqtt_client_init(&mqtt, config.mqtt_url, client_id, config.mqtt_username, config.mqtt_password, "", "", 30, NULL, NULL);
  } else {
    memset(&mqtt, 0, sizeof(mqtt));
    mqtt.socket_fd = -1;
    br_touch_logf("USB mode: actions will be written to %s", config.usb_touch_action_path);
  }
  br_touch_gesture_init(&gesture, config.swipe_threshold, config.long_press_ms);

#ifndef __linux__
  br_touch_logf("touch input requires Linux event devices");
  return 1;
#else
  br_touch_logf("start device=%s topic=%s broker=%s",
                config.touch_device,
                config.input_action_topic,
                config.mqtt_url);

  while (true) {
    if (!config.usb_mode && !mqtt.connected) {
      br_mqtt_client_ensure_connected(&mqtt, 30000);
    }
    if (input_fd < 0) {
      input_fd = open(config.touch_device, O_RDONLY | O_NONBLOCK);
      if (input_fd < 0) {
        br_touch_logf("open failed: %s", strerror(errno));
        br_sleep_ms(1000);
        continue;
      }
    }

    fd_set read_set;
    int max_fd = input_fd;
    struct timeval timeout;
    FD_ZERO(&read_set);
    FD_SET(input_fd, &read_set);
    if (!config.usb_mode && mqtt.connected && mqtt.socket_fd >= 0) {
      FD_SET(mqtt.socket_fd, &read_set);
      if (mqtt.socket_fd > max_fd) {
        max_fd = mqtt.socket_fd;
      }
    }
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;

    int ready = select(max_fd + 1, &read_set, NULL, NULL, &timeout);
    if (ready < 0) {
      br_touch_logf("select failed: %s", strerror(errno));
      close(input_fd);
      input_fd = -1;
      br_mqtt_client_close(&mqtt);
      br_sleep_ms(1000);
      continue;
    }

    if (!config.usb_mode && mqtt.connected) {
      br_mqtt_client_poll(&mqtt, 0);
    }

    if (FD_ISSET(input_fd, &read_set)) {
      unsigned char buffer[24 * 32];
      ssize_t read_size = read(input_fd, buffer, sizeof(buffer));
      if (read_size <= 0) {
        close(input_fd);
        input_fd = -1;
      } else {
        size_t position = 0;
        while (position + (size_t) config.event_size <= (size_t) read_size) {
          unsigned short type = 0;
          unsigned short code = 0;
          int value = 0;
          if (br_parse_input_event_packet(buffer + position, config.event_size, &type, &code, &value)) {
            br_handle_input_event(&gesture, &mqtt, &config, type, code, value);
          }
          position += (size_t) config.event_size;
        }
      }
    }
  }
#endif
}
