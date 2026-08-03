#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/gpio.h>
#include <sys/ioctl.h>
#ifndef GPIOHANDLE_REQUEST_BIAS_PULL_UP
#define GPIOHANDLE_REQUEST_BIAS_PULL_UP (1UL << 5)
#endif
#endif

#include "rotary_decoder.h"
#include "runtime_common.h"
#include "runtime_protocol.h"
#include "screen_page.h"
#include "voice_button.h"

typedef enum {
  BR_ROTARY_GPIO_BUTTON = 0,
  BR_ROTARY_GPIO_A = 1,
  BR_ROTARY_GPIO_B = 2,
  BR_ROTARY_GPIO_SW = 3,
  BR_ROTARY_GPIO_COUNT = 4
} br_rotary_gpio_index;

typedef enum {
  BR_GPIO_BACKEND_NONE = 0,
  BR_GPIO_BACKEND_SYSFS = 1,
  BR_GPIO_BACKEND_CHIP = 2
} br_gpio_backend;

typedef struct {
  char root_dir[BR_MAX_PATH];
  char screen_page_path[BR_MAX_PATH];
  char touch_request_path[BR_MAX_PATH];
  char voice_button_path[BR_MAX_PATH];
  char network_config_path[BR_MAX_PATH];
  char voice_button_config_path[BR_MAX_PATH];
  char button_config_path[BR_MAX_PATH];
  char voice_ptt_path[BR_MAX_PATH];
  char active_widget_path[BR_MAX_PATH];
  char widget_events_path[BR_MAX_PATH];
  char volume_display_path[BR_MAX_PATH];
  char clockwise_page[32];
  char counter_clockwise_page[32];
  int gpio_button;
  int gpio_a;
  int gpio_b;
  int gpio_rotary_button;
  int page_steps;
  int poll_ms;
  int debounce_ms;
  int long_press_ms;
  int rotary_button_long_press_ms;
  int rotary_reset_long_press_ms;
  int page_cooldown_ms;
  bool toggle_pages;
  bool button_touch_enabled;
  bool voice_button_enabled;
  char long_press_cmd[256];
  char rotary_reset_cmd[256];
  char audio_bridge_script_path[BR_MAX_PATH];
} br_rotary_config;

typedef struct {
  br_gpio_backend backend;
  int chip_fd;
  int handle_fd;
  const br_rotary_config *config;
} br_gpio_reader;

typedef struct {
  int level;
  long long pressed_at_ms;
  long long last_transition_ms;
  bool long_emitted;
  bool voice_ptt_active;
} br_button_state;

static void br_rotary_logf(const char *format, ...) {
  va_list args;
  fprintf(stdout, "[rotary-input] ");
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);
  fputc('\n', stdout);
  fflush(stdout);
}

static int br_env_int(const char *name, int fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  long parsed;
  if (!value || *value == '\0') {
    return fallback;
  }
  parsed = strtol(value, &end, 10);
  if (!end || *end != '\0') {
    return fallback;
  }
  return (int) parsed;
}

static void br_rotary_load_config(br_rotary_config *config, const char *root_dir) {
  br_normalize_text(root_dir, ".", config->root_dir, sizeof(config->root_dir));
  snprintf(config->screen_page_path, sizeof(config->screen_page_path), "%s/.screen-page", config->root_dir);
  snprintf(config->touch_request_path, sizeof(config->touch_request_path), "%s/.touch-request", config->root_dir);
  snprintf(config->voice_button_path, sizeof(config->voice_button_path), "%s/.voice-button-state", config->root_dir);
  snprintf(config->network_config_path, sizeof(config->network_config_path), "%s/network-config.json", config->root_dir);
  snprintf(config->voice_button_config_path, sizeof(config->voice_button_config_path), "%s/.voice-button-config", config->root_dir);
  snprintf(config->button_config_path, sizeof(config->button_config_path), "%s/.button-config", config->root_dir);
  snprintf(config->voice_ptt_path, sizeof(config->voice_ptt_path), "%s/.voice-ptt", config->root_dir);
  snprintf(config->active_widget_path, sizeof(config->active_widget_path), "%s/.active-widget", config->root_dir);
  snprintf(config->widget_events_path, sizeof(config->widget_events_path), "%s/.widget-events", config->root_dir);
  snprintf(config->volume_display_path, sizeof(config->volume_display_path), "%s/.volume-display", config->root_dir);
  snprintf(config->audio_bridge_script_path, sizeof(config->audio_bridge_script_path), "%s/board-audio-bridge.sh", config->root_dir);
  br_normalize_text(getenv("PET_ROTARY_CLOCKWISE_PAGE"), "stats", config->clockwise_page, sizeof(config->clockwise_page));
  br_normalize_text(getenv("PET_ROTARY_COUNTER_CLOCKWISE_PAGE"), "main", config->counter_clockwise_page, sizeof(config->counter_clockwise_page));
  config->gpio_button = br_env_int("PET_BUTTON_GPIO", 17);
  config->gpio_a = br_env_int("PET_ROTARY_GPIO_A", 23);
  config->gpio_b = br_env_int("PET_ROTARY_GPIO_B", 24);
  config->gpio_rotary_button = br_env_int("PET_ROTARY_GPIO_SW", 25);
  config->page_steps = br_env_int("PET_ROTARY_PAGE_STEPS", 2);
  if (config->page_steps <= 0) {
    config->page_steps = 2;
  }
  config->poll_ms = br_env_int("PET_ROTARY_POLL_MS", 5);
  if (config->poll_ms <= 0) {
    config->poll_ms = 5;
  }
  config->debounce_ms = br_env_int("PET_ROTARY_BUTTON_DEBOUNCE_MS", 200);
  if (config->debounce_ms < 0) {
    config->debounce_ms = 200;
  }
  config->long_press_ms = br_button_press_resolve_threshold_ms(
    br_env_int("PET_BUTTON_LONG_PRESS_MS", 0),
    1500);
  config->rotary_button_long_press_ms = br_button_press_resolve_threshold_ms(
    br_env_int("PET_ROTARY_BUTTON_LONG_PRESS_MS", 0),
    800);
  config->rotary_reset_long_press_ms = br_button_press_resolve_threshold_ms(
    br_env_int("PET_ROTARY_RESET_LONG_PRESS_MS", 0),
    8000);
  config->page_cooldown_ms = br_env_int("PET_ROTARY_PAGE_COOLDOWN_MS", 2500);
  if (config->page_cooldown_ms < 0) {
    config->page_cooldown_ms = 2500;
  }
  config->toggle_pages = br_env_int("PET_ROTARY_TOGGLE_PAGES", 1) != 0;
  config->button_touch_enabled = br_env_int("PET_ROTARY_BUTTON_TOUCH_ENABLED", 0) != 0;
  config->voice_button_enabled = br_env_int("PET_VOICE_BUTTON_ENABLED", 1) != 0;
  br_normalize_text(getenv("PET_BUTTON_LONG_PRESS_CMD"),
                    "systemctl restart board-runtime",
                    config->long_press_cmd,
                    sizeof(config->long_press_cmd));
  br_normalize_text(getenv("PET_ROTARY_RESET_CMD"),
                    "systemctl restart board-runtime || /etc/init.d/board-runtime restart",
                    config->rotary_reset_cmd,
                    sizeof(config->rotary_reset_cmd));
}

static void br_gpio_dir_path(int gpio, const char *name, char *output, size_t output_size) {
  snprintf(output, output_size, "/sys/class/gpio/gpio%d/%s", gpio, name);
}

static bool br_write_text_path(const char *path, const char *value) {
  int fd = open(path, O_WRONLY);
  size_t length;
  if (fd < 0) {
    return false;
  }
  length = strlen(value);
  if (write(fd, value, length) != (ssize_t) length) {
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

static bool br_gpio_export(int gpio) {
  char gpio_path[BR_MAX_PATH];
  char gpio_text[32];
  struct stat st;

  snprintf(gpio_path, sizeof(gpio_path), "/sys/class/gpio/gpio%d", gpio);
  if (stat(gpio_path, &st) == 0) {
    return true;
  }
  snprintf(gpio_text, sizeof(gpio_text), "%d", gpio);
  if (!br_write_text_path("/sys/class/gpio/export", gpio_text) && errno != EBUSY) {
    return false;
  }
  for (int attempt = 0; attempt < 20; attempt += 1) {
    if (stat(gpio_path, &st) == 0) {
      return true;
    }
    br_sleep_ms(25);
  }
  return false;
}

static bool br_gpio_prepare_input(int gpio) {
  char direction_path[BR_MAX_PATH];
  if (gpio < 0) {
    return true;
  }
  if (!br_gpio_export(gpio)) {
    br_rotary_logf("gpio%d export failed: %s", gpio, strerror(errno));
    return false;
  }
  br_gpio_dir_path(gpio, "direction", direction_path, sizeof(direction_path));
  if (!br_write_text_path(direction_path, "in")) {
    br_rotary_logf("gpio%d direction failed: %s", gpio, strerror(errno));
    return false;
  }
  return true;
}

static int br_gpio_number_for_index(const br_rotary_config *config, br_rotary_gpio_index index) {
  switch (index) {
    case BR_ROTARY_GPIO_BUTTON:
      return config->gpio_button;
    case BR_ROTARY_GPIO_A:
      return config->gpio_a;
    case BR_ROTARY_GPIO_B:
      return config->gpio_b;
    case BR_ROTARY_GPIO_SW:
      return config->gpio_rotary_button;
    default:
      return -1;
  }
}

static int br_gpio_read_sysfs_value(int gpio) {
  char value_path[BR_MAX_PATH];
  char value = '1';
  int fd;
  ssize_t read_size;

  if (gpio < 0) {
    return 1;
  }
  br_gpio_dir_path(gpio, "value", value_path, sizeof(value_path));
  fd = open(value_path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  read_size = read(fd, &value, 1);
  close(fd);
  if (read_size <= 0) {
    return -1;
  }
  return value == '0' ? 0 : 1;
}

#ifdef __linux__
static bool br_gpio_reader_init_chip(br_gpio_reader *reader, const br_rotary_config *config) {
  const char *chip_path = getenv("PET_GPIOCHIP") ? getenv("PET_GPIOCHIP") : "/dev/gpiochip0";
  struct gpiohandle_request request;
  int chip_fd;

  chip_fd = open(chip_path, O_RDONLY);
  if (chip_fd < 0) {
    br_rotary_logf("gpiochip open failed: %s (%s)", chip_path, strerror(errno));
    return false;
  }

  memset(&request, 0, sizeof(request));
  request.lineoffsets[BR_ROTARY_GPIO_BUTTON] = (unsigned int) config->gpio_button;
  request.lineoffsets[BR_ROTARY_GPIO_A] = (unsigned int) config->gpio_a;
  request.lineoffsets[BR_ROTARY_GPIO_B] = (unsigned int) config->gpio_b;
  request.lineoffsets[BR_ROTARY_GPIO_SW] = (unsigned int) config->gpio_rotary_button;
  request.lines = BR_ROTARY_GPIO_COUNT;
  request.flags = GPIOHANDLE_REQUEST_INPUT | GPIOHANDLE_REQUEST_BIAS_PULL_UP;
  snprintf(request.consumer_label, sizeof(request.consumer_label), "board-rotary-input");

  if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) != 0) {
    br_rotary_logf("gpiochip line request failed: %s", strerror(errno));
    close(chip_fd);
    return false;
  }

  reader->backend = BR_GPIO_BACKEND_CHIP;
  reader->chip_fd = chip_fd;
  reader->handle_fd = request.fd;
  reader->config = config;
  br_rotary_logf("using gpiochip backend: %s", chip_path);
  return true;
}
#endif

static bool br_gpio_reader_init(br_gpio_reader *reader, const br_rotary_config *config) {
  bool sysfs_ready;
  memset(reader, 0, sizeof(*reader));
  reader->backend = BR_GPIO_BACKEND_NONE;
  reader->chip_fd = -1;
  reader->handle_fd = -1;
  reader->config = config;

  sysfs_ready = br_gpio_prepare_input(config->gpio_a) &&
                br_gpio_prepare_input(config->gpio_b) &&
                br_gpio_prepare_input(config->gpio_button) &&
                br_gpio_prepare_input(config->gpio_rotary_button);
  if (sysfs_ready) {
    reader->backend = BR_GPIO_BACKEND_SYSFS;
    br_rotary_logf("using sysfs gpio backend");
    return true;
  }

#ifdef __linux__
  if (br_gpio_reader_init_chip(reader, config)) {
    return true;
  }
#endif

  return false;
}

static int br_gpio_reader_read(const br_gpio_reader *reader, br_rotary_gpio_index index) {
  if (!reader || !reader->config) {
    return -1;
  }
  if (reader->backend == BR_GPIO_BACKEND_SYSFS) {
    return br_gpio_read_sysfs_value(br_gpio_number_for_index(reader->config, index));
  }
#ifdef __linux__
  if (reader->backend == BR_GPIO_BACKEND_CHIP) {
    struct gpiohandle_data data;
    memset(&data, 0, sizeof(data));
    if (ioctl(reader->handle_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) != 0) {
      return -1;
    }
    return data.values[index] ? 1 : 0;
  }
#endif
  return -1;
}

static void br_rotary_emit_page(const br_rotary_config *config, const char *page, const char *reason) {
  if (br_atomic_write_text(config->screen_page_path, page)) {
    br_rotary_logf("screen-page=%s (%s)", page, reason);
  } else {
    br_rotary_logf("screen-page write failed: %s", config->screen_page_path);
  }
}

static void br_rotary_toggle_screen_page(const br_rotary_config *config, const char *reason) {
  char current_page[32];
  char next_page[32];

  if (!br_read_text_file(config->screen_page_path, current_page, sizeof(current_page))) {
    current_page[0] = '\0';
  }
  if (br_screen_page_toggle_main_stats(current_page, next_page, sizeof(next_page))) {
    br_rotary_emit_page(config, next_page, reason);
  }
}

static void br_rotary_trim(char *value) {
  size_t len;
  if (!value) {
    return;
  }
  len = strlen(value);
  while (len > 0 &&
         (value[len - 1] == '\n' || value[len - 1] == '\r' ||
          value[len - 1] == ' ' || value[len - 1] == '\t')) {
    value[--len] = '\0';
  }
}

static bool br_rotary_widget_active_on_stats(const br_rotary_config *config) {
  char widget[128];
  char page[32];
  if (!config ||
      !br_read_text_file(config->active_widget_path, widget, sizeof(widget)) ||
      !br_read_text_file(config->screen_page_path, page, sizeof(page))) {
    return false;
  }
  br_rotary_trim(widget);
  br_rotary_trim(page);
  return widget[0] != '\0' && strcmp(page, "stats") == 0;
}

static bool br_rotary_read_button_action(
  const br_rotary_config *config,
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

static bool br_rotary_append_widget_event(
  const br_rotary_config *config,
  const char *control,
  const char *event
) {
  FILE *file;
  if (!config || !control || !event) {
    return false;
  }
  if (!br_rotary_widget_active_on_stats(config)) {
    br_rotary_logf("widget event skipped: no active stats widget (%s)", event);
    return true;
  }
  file = fopen(config->widget_events_path, "a");
  if (!file) {
    br_rotary_logf("widget event open failed: %s", config->widget_events_path);
    return true;
  }
  fprintf(file, "{\"control\":\"%s\",\"event\":\"%s\",\"ts\":%lld}\n",
          control,
          event,
          br_now_ms());
  fclose(file);
  br_rotary_logf("widget event: %s %s", control, event);
  return true;
}

static void br_rotary_run_command(const char *label, const char *command) {
  int exit_code;
  if (!command || !command[0]) {
    br_rotary_logf("%s skipped: empty command", label);
    return;
  }
  br_rotary_logf("%s command: %s", label, command);
  exit_code = system(command);
  if (exit_code != 0) {
    br_rotary_logf("%s command exited with %d", label, exit_code);
  }
}

static void br_rotary_restart_service(const br_rotary_config *config) {
  br_rotary_run_command("long press restart", config->long_press_cmd);
}

static void br_rotary_reset_pairing(const br_rotary_config *config) {
  if (unlink(config->network_config_path) == 0) {
    br_rotary_logf("pairing reset deleted %s", config->network_config_path);
  } else if (errno == ENOENT) {
    br_rotary_logf("pairing reset config already missing: %s", config->network_config_path);
  } else {
    br_rotary_logf("pairing reset unlink failed: %s (%s)",
                   config->network_config_path,
                   strerror(errno));
  }
  br_rotary_run_command("pairing reset", config->rotary_reset_cmd);
}

static bool br_rotary_dispatch_button_action(
  const br_rotary_config *config,
  const char *event,
  const char *default_reset_label
) {
  char action[64];
  if (!br_rotary_read_button_action(config, event, action, sizeof(action))) {
    return false;
  }
  if (strcmp(action, "disabled") == 0 || strcmp(action, "voice_ptt") == 0) {
    br_rotary_logf("button action %s -> %s", event, action);
    return true;
  }
  if (strcmp(action, "system_page") == 0) {
    br_rotary_toggle_screen_page(config, event);
    return true;
  }
  if (strcmp(action, "system_reset") == 0) {
    if (default_reset_label && strcmp(default_reset_label, "pairing") == 0) {
      br_rotary_reset_pairing(config);
    } else {
      br_rotary_restart_service(config);
    }
    return true;
  }
  return false;
}

/* Adjust the system-wide ALSA softvol "Master" control (set up in
   /etc/asound.conf) by one step and publish the new level to
   .volume-display for fb-display's on-screen volume bar. The VoiceHAT
   MAX98357A has no hardware mixer, so this software control is the only
   volume knob. One shelled-out amixer set+get per detent is acceptable;
   a 120ms debounce protects the CPU-bound board from rapid spins. */
static void br_rotary_adjust_volume(
  const br_rotary_config *config,
  br_rotary_direction direction
) {
  static long long last_volume_ms = 0;
  long long now_ms = br_now_ms();
  /* Clockwise raises volume. The decoder's BR_ROTARY_CLOCKWISE corresponds to
     the physical counter-clockwise detent on this knob's A/B wiring, so map it
     to the decrease step. */
  const char *delta = (direction == BR_ROTARY_CLOCKWISE) ? "6%-" : "6%+";
  char cmd[256];
  FILE *fp;
  int pct = -1;

  if (last_volume_ms > 0 && (now_ms - last_volume_ms) < 120) {
    return; /* debounce rapid spins */
  }
  last_volume_ms = now_ms;

  /* set then read back the mapped percentage in one shell to limit spawns.
     %%+/-%% and %% are literal percent signs after snprintf expansion. */
  snprintf(cmd, sizeof(cmd),
           "amixer -D default sset Master %s -M >/dev/null 2>&1; "
           "amixer -D default sget Master 2>/dev/null | grep -om1 '[0-9]\\+%%' | tr -d '%%'",
           delta);
  fp = popen(cmd, "r");
  if (fp) {
    char buf[16] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
      pct = atoi(buf);
    }
    pclose(fp);
  }
  if (pct < 0) {
    br_rotary_logf("volume adjust failed (%s)", delta);
    return;
  }
  {
    char payload[64];
    snprintf(payload, sizeof(payload), "%d\n%lld\n", pct, now_ms);
    br_atomic_write_text(config->volume_display_path, payload);
  }
  br_rotary_logf("volume %s -> %d%%",
                 direction == BR_ROTARY_CLOCKWISE ? "up" : "down", pct);
}

static void br_rotary_emit_direction(
  const br_rotary_config *config,
  br_rotary_direction direction,
  long long *last_page_ms
) {
  char current_page[32];
  char next_page[32];
  const char *reason = direction == BR_ROTARY_CLOCKWISE ? "rotary_cw" : "rotary_ccw";
  long long now_ms = br_now_ms();
  char configured_action[64];

  if (br_rotary_read_button_action(config,
                                   "knob.rotate_cw / knob.rotate_ccw",
                                   configured_action,
                                   sizeof(configured_action))) {
    if (strcmp(configured_action, "disabled") == 0) {
      br_rotary_logf("rotary direction ignored by button_config");
      return;
    }
    if (strcmp(configured_action, "volume_adjust") == 0) {
      br_rotary_adjust_volume(config, direction);
      return;
    }
    if (strcmp(configured_action, "system_page") != 0) {
      br_rotary_logf("rotary direction ignored by unsupported button_config action: %s", configured_action);
      return;
    }
    /* configured_action == "system_page": fall through to page toggle below. */
  } else {
    /* No knob.rotate binding configured → default to volume control per the
       2026-06-01 knob-volume spec. Page switching stays available on swipe. */
    br_rotary_adjust_volume(config, direction);
    return;
  }

  if (config->page_cooldown_ms > 0 && *last_page_ms > 0 &&
      (now_ms - *last_page_ms) < config->page_cooldown_ms) {
    br_rotary_logf("screen-page skipped (%s cooldown)", reason);
    return;
  }

  if (!br_read_text_file(config->screen_page_path, current_page, sizeof(current_page))) {
    current_page[0] = '\0';
  }
  if (!br_rotary_select_page(current_page,
                             config->clockwise_page,
                             config->counter_clockwise_page,
                             config->toggle_pages,
                             direction,
                             next_page,
                             sizeof(next_page))) {
    return;
  }
  br_rotary_emit_page(config, next_page, reason);
  *last_page_ms = now_ms;
}

static void br_rotary_emit_button(const br_rotary_config *config, const char *name) {
  char marker[128];
  snprintf(marker, sizeof(marker), "%lld %s\n", br_now_ms(), name);
  if (br_atomic_write_text(config->touch_request_path, marker)) {
    br_rotary_logf("local touch request=%s", name);
  }
}

static void br_rotary_emit_voice_button(const br_rotary_config *config, const char *state) {
  char marker[128];
  if (!config || !config->voice_button_enabled || !state || state[0] == '\0') {
    return;
  }
  snprintf(marker, sizeof(marker), "%lld %s\n", br_now_ms(), state);
  if (!br_atomic_write_text(config->voice_button_path, marker)) {
    br_rotary_logf("voice-button write failed: %s", config->voice_button_path);
    return;
  }
  br_rotary_logf("voice-button %s", state);
}

static bool br_rotary_read_voice_button(const br_rotary_config *config, char *output, size_t output_size) {
  char value[96];
  if (!config || !output || output_size == 0) {
    return false;
  }
  output[0] = '\0';
  if (!br_read_text_file(config->voice_button_config_path, value, sizeof(value))) {
    return false;
  }
  return br_voice_button_normalize(value, output, output_size);
}

static bool br_rotary_voice_button_matches(const br_rotary_config *config, const char *button) {
  char configured[64];
  if (!br_rotary_read_voice_button(config, configured, sizeof(configured))) {
    return false;
  }
  return strcmp(configured, button) == 0;
}

static bool br_rotary_action_is_voice_ptt(const br_rotary_config *config, const char *event) {
  char action[64];
  return br_rotary_read_button_action(config, event, action, sizeof(action)) &&
         strcmp(action, "voice_ptt") == 0;
}

static bool br_rotary_has_button_action(const br_rotary_config *config, const char *event) {
  char action[64];
  return br_rotary_read_button_action(config, event, action, sizeof(action));
}

static bool br_rotary_should_start_voice_ptt(
  const br_rotary_config *config,
  const char *short_event,
  const char *long_event,
  const char *voice_button
) {
  bool has_short = short_event && br_rotary_has_button_action(config, short_event);
  bool has_long = long_event && br_rotary_has_button_action(config, long_event);
  if (has_short || has_long) {
    return short_event &&
           br_rotary_action_is_voice_ptt(config, short_event) &&
           br_rotary_voice_button_matches(config, voice_button);
  }
  return br_rotary_voice_button_matches(config, voice_button);
}

static void br_rotary_set_voice_ptt(
  const br_rotary_config *config,
  const char *button,
  bool active
) {
  char marker[160];
  char command[BR_MAX_PATH + 160];
  const char *action = active ? "ptt-start" : "ptt-stop";
  snprintf(marker, sizeof(marker), "%lld %s %s\n", br_now_ms(), active ? "start" : "stop", button);
  if (br_atomic_write_text(config->voice_ptt_path, marker)) {
    br_rotary_logf("voice ptt %s button=%s", active ? "start" : "stop", button);
  }
  br_rotary_emit_voice_button(config, active ? "down" : "up");
  snprintf(command, sizeof(command), "sh '%s' %s '%s'",
           config->audio_bridge_script_path,
           action,
           config->root_dir);
  br_rotary_run_command(active ? "voice ptt start" : "voice ptt stop", command);
}

static void br_rotary_check_primary_button(
  const br_gpio_reader *reader,
  const br_rotary_config *config,
  br_button_state *state
) {
  int level;
  long long now_ms;
  level = br_gpio_reader_read(reader, BR_ROTARY_GPIO_BUTTON);
  if (level < 0) {
    return;
  }
  now_ms = br_now_ms();

  if (level != state->level) {
    if (config->debounce_ms > 0 &&
        state->last_transition_ms > 0 &&
        (now_ms - state->last_transition_ms) < config->debounce_ms) {
      return;
    }
    state->last_transition_ms = now_ms;

    if (state->level == 1 && level == 0) {
      state->pressed_at_ms = now_ms;
      state->long_emitted = false;
      state->voice_ptt_active = br_rotary_should_start_voice_ptt(config,
                                                                 "button.primary.short_press",
                                                                 "button.primary.long_press",
                                                                 BR_VOICE_BUTTON_TOP_HOLD);
      if (state->voice_ptt_active) {
        br_rotary_set_voice_ptt(config, BR_VOICE_BUTTON_TOP_HOLD, true);
      }
      br_rotary_logf("button down");
    } else if (state->level == 0 && level == 1) {
      long long duration_ms = state->pressed_at_ms > 0 ? now_ms - state->pressed_at_ms : 0;
      if (state->voice_ptt_active) {
        br_rotary_set_voice_ptt(config, BR_VOICE_BUTTON_TOP_HOLD, false);
        state->voice_ptt_active = false;
      } else if (!state->long_emitted) {
        br_button_press_kind press_kind = br_button_press_classify(duration_ms, config->long_press_ms);
        const char *event = press_kind == BR_BUTTON_PRESS_LONG
          ? "button.primary.long_press" : "button.primary.short_press";
        if (br_rotary_dispatch_button_action(config, event, "runtime")) {
          state->long_emitted = press_kind == BR_BUTTON_PRESS_LONG;
        } else {
          br_primary_button_action action =
            br_primary_button_resolve_action(press_kind, false);
          if (action == BR_PRIMARY_BUTTON_RESTART_RUNTIME) {
            state->long_emitted = true;
            br_rotary_restart_service(config);
          } else {
            br_rotary_toggle_screen_page(config, "button_short");
          }
        }
      }
      br_rotary_logf("button up duration=%lldms action=%s",
                     duration_ms,
                     state->long_emitted ? "long" : "short");
      state->pressed_at_ms = 0;
    }

    state->level = level;
  }

  if (state->level == 0 &&
      !state->voice_ptt_active &&
      !state->long_emitted &&
      state->pressed_at_ms > 0 &&
      br_button_press_classify(now_ms - state->pressed_at_ms, config->long_press_ms) == BR_BUTTON_PRESS_LONG) {
    if (!br_rotary_has_button_action(config, "button.primary.long_press")) {
      state->long_emitted = true;
      br_rotary_restart_service(config);
    }
  }
}

static void br_rotary_check_rotary_button(
  const br_gpio_reader *reader,
  const br_rotary_config *config,
  br_button_state *state
) {
  int level;
  long long now_ms;
  bool has_long_action;
  int long_press_ms;
  level = br_gpio_reader_read(reader, BR_ROTARY_GPIO_SW);
  if (level < 0) {
    return;
  }
  now_ms = br_now_ms();
  has_long_action = br_rotary_has_button_action(config, "button.encoder.long_press");
  long_press_ms = has_long_action
    ? config->rotary_button_long_press_ms
    : config->rotary_reset_long_press_ms;

  if (level != state->level) {
    if (config->debounce_ms > 0 &&
        state->last_transition_ms > 0 &&
        (now_ms - state->last_transition_ms) < config->debounce_ms) {
      return;
    }
    state->last_transition_ms = now_ms;

    if (state->level == 1 && level == 0) {
      state->pressed_at_ms = now_ms;
      state->long_emitted = false;
      state->voice_ptt_active = br_rotary_should_start_voice_ptt(config,
                                                                 "button.encoder.short_press",
                                                                 "button.encoder.long_press",
                                                                 BR_VOICE_BUTTON_ENCODER_HOLD);
      if (state->voice_ptt_active) {
        br_rotary_set_voice_ptt(config, BR_VOICE_BUTTON_ENCODER_HOLD, true);
      }
      br_rotary_logf("rotary button down");
    } else if (state->level == 0 && level == 1) {
      long long duration_ms = state->pressed_at_ms > 0 ? now_ms - state->pressed_at_ms : 0;
      if (state->voice_ptt_active) {
        br_rotary_set_voice_ptt(config, BR_VOICE_BUTTON_ENCODER_HOLD, false);
        state->voice_ptt_active = false;
      } else if (!state->long_emitted) {
        bool is_long = br_button_press_classify(duration_ms, long_press_ms) == BR_BUTTON_PRESS_LONG;
        const char *event = is_long ? "button.encoder.long_press" : "button.encoder.short_press";
        if (br_rotary_dispatch_button_action(config, event, "pairing")) {
          state->long_emitted = is_long;
        } else if (is_long) {
          state->long_emitted = true;
          br_rotary_reset_pairing(config);
        } else if (config->button_touch_enabled) {
          br_rotary_emit_button(config, "rotary_button");
        } else {
          br_rotary_logf("button pressed=rotary_button");
        }
      }
      br_rotary_logf("rotary button up duration=%lldms action=%s",
                     duration_ms,
                     state->long_emitted ? "reset_pairing" : "short");
      state->pressed_at_ms = 0;
    }

    state->level = level;
  }

  if (state->level == 0 &&
      !state->voice_ptt_active &&
      !state->long_emitted &&
      state->pressed_at_ms > 0 &&
      br_button_press_classify(now_ms - state->pressed_at_ms, long_press_ms) == BR_BUTTON_PRESS_LONG) {
    if (br_rotary_action_is_voice_ptt(config, "button.encoder.long_press") &&
        br_rotary_voice_button_matches(config, BR_VOICE_BUTTON_ENCODER_HOLD)) {
      state->voice_ptt_active = true;
      state->long_emitted = true;
      br_rotary_set_voice_ptt(config, BR_VOICE_BUTTON_ENCODER_HOLD, true);
    } else if (!has_long_action) {
      state->long_emitted = true;
      br_rotary_reset_pairing(config);
    } else if (br_rotary_dispatch_button_action(config, "button.encoder.long_press", "pairing")) {
      state->long_emitted = true;
    }
  }
}

int main(int argc, char **argv) {
  br_rotary_config config;
  br_gpio_reader gpio;
  br_rotary_decoder decoder;
  int a_level;
  int b_level;
  int button_level = 1;
  int rotary_button_level = 1;
  long long last_page_ms = 0;
  br_button_state primary_button;
  br_button_state rotary_button;

  if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    printf("usage: %s [runtime-root]\n", argv[0]);
    return 0;
  }

  br_rotary_load_config(&config, argc > 1 ? argv[1] : ".");
  if (!br_gpio_reader_init(&gpio, &config)) {
    br_rotary_logf("no usable GPIO backend");
    return 1;
  }

  a_level = br_gpio_reader_read(&gpio, BR_ROTARY_GPIO_A);
  b_level = br_gpio_reader_read(&gpio, BR_ROTARY_GPIO_B);
  if (a_level < 0 || b_level < 0) {
    br_rotary_logf("failed to read rotary A/B levels");
    return 1;
  }
  button_level = br_gpio_reader_read(&gpio, BR_ROTARY_GPIO_BUTTON);
  rotary_button_level = br_gpio_reader_read(&gpio, BR_ROTARY_GPIO_SW);
  if (button_level < 0) {
    button_level = 1;
  }
  if (rotary_button_level < 0) {
    rotary_button_level = 1;
  }
  br_rotary_decoder_init(&decoder, a_level, b_level, config.page_steps);
  memset(&primary_button, 0, sizeof(primary_button));
  primary_button.level = button_level;
  if (button_level == 0) {
    primary_button.pressed_at_ms = br_now_ms();
  }
  memset(&rotary_button, 0, sizeof(rotary_button));
  rotary_button.level = rotary_button_level;
  if (rotary_button_level == 0) {
    rotary_button.pressed_at_ms = br_now_ms();
  }

  br_rotary_logf("start gpioA=%d gpioB=%d button=%d rotaryButton=%d steps=%d cooldown=%d longPress=%d rotaryButtonLongPress=%d rotaryResetLongPress=%d toggle=%d buttonTouch=%d pages cw=%s ccw=%s",
                 config.gpio_a,
                 config.gpio_b,
                 config.gpio_button,
                 config.gpio_rotary_button,
                 config.page_steps,
                 config.page_cooldown_ms,
                 config.long_press_ms,
                 config.rotary_button_long_press_ms,
                 config.rotary_reset_long_press_ms,
                 config.toggle_pages ? 1 : 0,
                 config.button_touch_enabled ? 1 : 0,
                 config.clockwise_page,
                 config.counter_clockwise_page);
  br_rotary_logf("voice-button enabled=%d path=%s",
                 config.voice_button_enabled ? 1 : 0,
                 config.voice_button_path);

  while (true) {
    br_rotary_direction direction;
    a_level = br_gpio_reader_read(&gpio, BR_ROTARY_GPIO_A);
    b_level = br_gpio_reader_read(&gpio, BR_ROTARY_GPIO_B);
    if (a_level >= 0 && b_level >= 0) {
      direction = br_rotary_decoder_update(&decoder, a_level, b_level);
      if (direction != BR_ROTARY_NONE) {
        br_rotary_emit_direction(&config, direction, &last_page_ms);
      }
    }

    br_rotary_check_primary_button(&gpio, &config, &primary_button);
    br_rotary_check_rotary_button(&gpio, &config, &rotary_button);
    br_sleep_ms(config.poll_ms);
  }
}
