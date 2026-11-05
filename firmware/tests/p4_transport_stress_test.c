#define DRAM_ATTR
/* Host-only production TX test. Driver queues are bounded and asynchronously
 * drained; no serial device, production policy change, or wall-time claim. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#define PET_P4_UART_NUM 0
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0
#define pdTRUE 1
typedef uint32_t TickType_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static pthread_mutex_t transport_mutex = PTHREAD_MUTEX_INITIALIZER;
static SemaphoreHandle_t g_transport_mutex = &transport_mutex;
static bool g_transport_ready = true;
static char g_transport_log_buffer[1024];
typedef enum { PET_P4_TRANSPORT_BROADCAST, PET_P4_TRANSPORT_UART,
  PET_P4_TRANSPORT_USB_SERIAL_JTAG } pet_p4_transport_route_t;
static struct { void *raw; int resources; } g_screenshot = {(void *)1, 0};
static atomic_bool audio_busy;
static atomic_uint fake_tick;
static _Thread_local bool audio_sender;
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  pthread_t thread;
  char queue[97], wire[1024 * 1024];
  size_t head, queued, used, accepted, peak_queued;
  unsigned writes, drains;
  long fail_after;
  int fail_return;
  bool stop, drain_error, expire_after_write;
} driver_t;
static driver_t uart_driver, usb_driver;

static void *drain_driver(void *context) {
  driver_t *d = context;
  assert(!pthread_mutex_lock(&d->lock));
  for (;;) {
    while (!d->queued && !d->stop) assert(!pthread_cond_wait(&d->changed, &d->lock));
    if (!d->queued && d->stop) break;
    size_t n = d->queued < 13 ? d->queued : 13;
    assert(d->used + n < sizeof(d->wire));
    for (size_t i = 0; i < n; i++) {
      d->wire[d->used++] = d->queue[d->head];
      d->head = (d->head + 1) % sizeof(d->queue);
    }
    d->queued -= n;
    d->wire[d->used] = 0;
    assert(!pthread_cond_broadcast(&d->changed));
    assert(!pthread_mutex_unlock(&d->lock));
    sched_yield();
    assert(!pthread_mutex_lock(&d->lock));
  }
  assert(!pthread_mutex_unlock(&d->lock));
  return NULL;
}
static void start_driver(driver_t *d) {
  assert(!pthread_mutex_init(&d->lock, NULL));
  assert(!pthread_cond_init(&d->changed, NULL));
  d->fail_after = -1;
  assert(!pthread_create(&d->thread, NULL, drain_driver, d));
}
static void flush_driver(driver_t *d) {
  assert(!pthread_mutex_lock(&d->lock));
  while (d->queued) assert(!pthread_cond_wait(&d->changed, &d->lock));
  assert(!pthread_mutex_unlock(&d->lock));
}
static void reset_driver(driver_t *d) {
  flush_driver(d);
  assert(!pthread_mutex_lock(&d->lock));
  d->head = d->used = d->accepted = d->peak_queued = 0;
  d->writes = d->drains = 0;
  d->fail_after = -1; d->fail_return = 0;
  d->drain_error = d->expire_after_write = false;
  d->wire[0] = 0;
  assert(!pthread_mutex_unlock(&d->lock));
}
static int driver_write(driver_t *d, const char *data, size_t n, size_t quantum) {
  assert(!pthread_mutex_lock(&d->lock));
  d->writes++;
  if (d->fail_after >= 0 && d->accepted >= (size_t)d->fail_after) {
    int result = d->fail_return;
    assert(!pthread_mutex_unlock(&d->lock));
    return result;
  }
  if (n > quantum) n = quantum;
  if (d->fail_after >= 0 && n > (size_t)d->fail_after - d->accepted)
    n = (size_t)d->fail_after - d->accepted;
  while (d->queued == sizeof(d->queue)) assert(!pthread_cond_wait(&d->changed, &d->lock));
  if (n > sizeof(d->queue) - d->queued) n = sizeof(d->queue) - d->queued;
  for (size_t i = 0; i < n; i++) d->queue[(d->head + d->queued + i) % sizeof(d->queue)] = data[i];
  d->queued += n; d->accepted += n;
  if (d->queued > d->peak_queued) d->peak_queued = d->queued;
  if (d->expire_after_write) atomic_store(&fake_tick, 101);
  assert(!pthread_cond_broadcast(&d->changed));
  assert(!pthread_mutex_unlock(&d->lock));
  sched_yield();
  return (int)n;
}
static int driver_drain(driver_t *d) {
  assert(!audio_sender); /* No explicit driver drain on the audio ACK path. */
  d->drains++;
  flush_driver(d);
  return d->drain_error ? -1 : ESP_OK;
}
static int xSemaphoreTake(SemaphoreHandle_t mutex, int wait) {
  (void)wait; assert(!pthread_mutex_lock(mutex)); return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(!pthread_mutex_unlock(mutex)); }
static bool usb_protocol_seen(void) { return true; }
static int uart_write_bytes(int port, const void *data, size_t n) {
  (void)port; return driver_write(&uart_driver, data, n, 17);
}
static int usb_serial_jtag_write_bytes(const char *data, size_t n, int timeout) {
  (void)timeout; return driver_write(&usb_driver, data, n, 11);
}
static int uart_wait_tx_done(int port, int timeout) {
  (void)port; (void)timeout; return driver_drain(&uart_driver);
}
static int usb_serial_jtag_wait_tx_done(int timeout) {
  (void)timeout; return driver_drain(&usb_driver);
}
static void pet_p4_native_usb_send_json_line(const char *line, void *ctx) { (void)line; (void)ctx; }
static bool screenshot_audio_busy(void) { return atomic_load(&audio_busy); }
static TickType_t xTaskGetTickCount(void) { return atomic_load(&fake_tick); }
static bool pet_resources_device_debug_current(const int *resources, bool busy) { (void)resources; return !busy; }
static int forbidden_console(const char *format, va_list args) { (void)format; (void)args; assert(false); return 0; }
#define vprintf forbidden_console
#include "production_transport.inc"

static char diagnostic[2200], hello[5300], screenshot_body[4097], screenshot_frame[4300];
static const char *shot_header = "{\"topic\":\"debug/screenshot_chunk\",\"payload\":{\"data\":\"";
static const char *shot_suffix = "\"}}\n";
static const char *audio_ack = "{\"topic\":\"audio/ack\",\"payload\":{\"ok\":true}}";
static void fill_json(char *out, size_t size, const char *topic, unsigned length, char value) {
  int n = snprintf(out, size, "{\"topic\":\"%s\",\"payload\":{\"data\":\"", topic);
  assert(n > 0 && (size_t)n + length + 4 < size);
  memset(out + n, value, length); strcpy(out + n + length, "\"}}");
}
static int log_line(const char *format, ...) {
  va_list args; va_start(args, format); int n = transport_log_vprintf(format, args); va_end(args); return n;
}
static void *long_frames(void *unused) {
  (void)unused;
  for (unsigned i = 0; i < 20; i++) { transport_send_line(diagnostic, NULL); transport_send_line(hello, NULL); }
  return NULL;
}
static void *screenshots(void *unused) {
  (void)unused;
  for (unsigned i = 0; i < 20; i++) assert(screenshot_send_parts(shot_header, screenshot_body, 4096, shot_suffix, NULL));
  return NULL;
}
static void *audio_acks(void *unused) {
  (void)unused; audio_sender = true;
  for (unsigned i = 0; i < 200; i++) {
    if (i % 2) transport_send_audio_line(audio_ack, NULL);
    else transport_send_line(audio_ack, NULL);
  }
  return NULL;
}
static void *logs(void *unused) {
  (void)unused; for (unsigned i = 0; i < 200; i++) log_line("I bounded transport sample=%u\n", i); return NULL;
}
static void successful_concurrency(void) {
  pthread_t producers[4];
  void *(*functions[])(void *) = {long_frames, screenshots, audio_acks, logs};
  for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&producers[i], NULL, functions[i], NULL));
  for (unsigned i = 0; i < 4; i++) assert(!pthread_join(producers[i], NULL));
  flush_driver(&uart_driver); flush_driver(&usb_driver);
  assert(uart_driver.used == usb_driver.used && !memcmp(uart_driver.wire, usb_driver.wire, uart_driver.used));
  assert(uart_driver.peak_queued <= 97 && usb_driver.peak_queued <= 97);
  unsigned counts[5] = {0}; bool log_seen[200] = {false}; char *save;
  for (char *line = strtok_r(uart_driver.wire, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (!strcmp(line, diagnostic)) counts[0]++;
    else if (!strcmp(line, hello)) counts[1]++;
    else if (!strcmp(line, screenshot_frame)) counts[2]++;
    else if (!strcmp(line, audio_ack)) counts[3]++;
    else {
      unsigned index; int n = 0;
      assert(sscanf(line, "I bounded transport sample=%u%n", &index, &n) == 1 && !line[n] && index < 200);
      assert(!log_seen[index]); log_seen[index] = true; counts[4]++;
    }
  }
  assert(counts[0] == 20 && counts[1] == 20 && counts[2] == 20 && counts[3] == 200 && counts[4] == 200);
  assert(uart_driver.drains == 100 && usb_driver.drains == 100);
}
static void failure_boundaries(void) {
  for (unsigned route_index = 0; route_index < 2; route_index++) {
    pet_p4_transport_route_t route = route_index ? PET_P4_TRANSPORT_USB_SERIAL_JTAG : PET_P4_TRANSPORT_UART;
    driver_t *d = route_index ? &usb_driver : &uart_driver;
    for (int failure = -1; failure <= 0; failure++) {
      reset_driver(&uart_driver); reset_driver(&usb_driver);
      d->fail_after = 23; d->fail_return = failure;
      /* Existing normal callback is void and cannot report partial/unknown.
       * This assertion documents that boundary; it does not endorse/retry it. */
      transport_send_line(diagnostic, &route); flush_driver(d);
      assert(d->used == 23 && !memchr(d->wire, '\n', d->used));
      reset_driver(d); d->fail_after = 23; d->fail_return = failure;
      assert(!screenshot_send_parts(shot_header, screenshot_body, 4096, shot_suffix, &route));
      flush_driver(d); assert(d->used == 23);
      d->fail_after = -1;
      screenshot_error("", "write_failed", 0, 23, &route); flush_driver(d);
      assert(d->wire[23] == '\n' && !strncmp(d->wire + 24, "{\"topic\":\"debug/screenshot_error\"", 33));
    }
    reset_driver(d); d->drain_error = true;
    assert(!screenshot_send_parts(shot_header, screenshot_body, 4096, shot_suffix, &route));
    flush_driver(d); assert(d->used == strlen(screenshot_frame) + 1); /* Already sent, not replayable. */
    reset_driver(d); d->expire_after_write = true; atomic_store(&fake_tick, 0);
    assert(!screenshot_send_line(diagnostic, &route));
    flush_driver(d); assert(d->used > 0 && d->used < strlen(diagnostic));
    atomic_store(&fake_tick, 0); reset_driver(d);
  }
  atomic_store(&audio_busy, true);
  assert(!screenshot_send_line(diagnostic, NULL));
  assert(!uart_driver.used && !usb_driver.used);
  atomic_store(&audio_busy, false);
}
int main(void) {
  fill_json(diagnostic, sizeof(diagnostic), "diagnostics/status", 2048, 'D');
  fill_json(hello, sizeof(hello), "device/hello", 5120, 'H');
  memset(screenshot_body, 'A', 4096);
  int n = snprintf(screenshot_frame, sizeof(screenshot_frame), "%s%s\"}}", shot_header, screenshot_body);
  assert(n > 0 && (size_t)n < sizeof(screenshot_frame));
  start_driver(&uart_driver); start_driver(&usb_driver);
  successful_concurrency(); failure_boundaries();
  driver_t *drivers[] = {&uart_driver, &usb_driver};
  for (unsigned i = 0; i < 2; i++) {
    driver_t *d = drivers[i]; flush_driver(d);
    assert(!pthread_mutex_lock(&d->lock)); d->stop = true;
    assert(!pthread_cond_signal(&d->changed)); assert(!pthread_mutex_unlock(&d->lock));
    assert(!pthread_join(d->thread, NULL));
    assert(!pthread_cond_destroy(&d->changed)); assert(!pthread_mutex_destroy(&d->lock));
  }
  puts("PASS production TX: long mixed records, bounded async queues, short/error writes, checked screenshot failure, audio no-drain path");
  return 0;
}
