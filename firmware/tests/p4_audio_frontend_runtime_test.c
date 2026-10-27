/* Execute production lifecycle/queue logic with deterministic DSP, RTOS and
 * allocation substitutes. This tests recovery, NOT acoustic AEC quality. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/pet_p4_audio_frontend.c"

static int allocations, fail_after = -1, dsp_creates;
static int test_chunk_samples = 320;
static uint64_t clock_us = 1, process_us;
static void *allocate(size_t size) {
  if (fail_after == 0) { fail_after = -1; return NULL; }
  if (fail_after > 0) --fail_after;
  void *pointer = calloc(1, size);
  assert(pointer); ++allocations; return pointer;
}
void heap_caps_free(void *pointer) { if (pointer) { --allocations; free(pointer); } }
void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return allocate(size); }
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) { return heap_caps_malloc(count * size, caps); }
void *heap_caps_aligned_alloc(size_t align, size_t size, unsigned caps) { (void)align; return heap_caps_malloc(size, caps); }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 100000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 100000; }
int64_t esp_timer_get_time(void) { return clock_us; }
aec_handle_t *aec_create_from_config(aec_config_t *config) {
  assert(config->caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#if CONFIG_ESP32P4_REV_MIN_FULL >= 300
  assert(config->mode == AEC_MODE_FD_HIGH_PERF);
#else
  assert(config->mode == AEC_MODE_VOIP_LOW_COST);
#endif
  ++dsp_creates;
  aec_handle_t *handle = allocate(sizeof(*handle));
  if (handle) handle->config = *config;
  return handle;
}
void aec_destroy(aec_handle_t *handle) { assert(handle); heap_caps_free(handle); }
int aec_get_chunksize(aec_handle_t *handle) { assert(handle); return test_chunk_samples; }
void aec_process(const aec_handle_t *handle, int16_t *mic, int16_t *ref, int16_t *out) {
  assert(handle); (void)ref; memcpy(out, mic, test_chunk_samples * sizeof(*out)); clock_us += process_us;
}
ns_handle_t ns_create(int ms) { assert(ms == 10); return allocate(1); }
void ns_destroy(ns_handle_t handle) { assert(handle); heap_caps_free(handle); }
void ns_process(ns_handle_t handle, int16_t *in, int16_t *out) { assert(handle); memcpy(out, in, 320); }
vad_handle_t vad_create_with_param(vad_mode_t mode, int rate, int ms, int speech, int silence) {
  assert(mode == VAD_MODE_2 && rate == 16000 && ms == 20 && speech == 120 && silence == 100);
  return allocate(1);
}
void vad_destroy(vad_handle_t handle) { assert(handle); heap_caps_free(handle); }
vad_state_t vad_process_with_trigger(vad_handle_t handle, int16_t *in) { assert(handle); return in[0] ? VAD_SPEECH : VAD_SILENCE; }
QueueHandle_t xQueueCreateStatic(size_t cap, size_t size, uint8_t *data, StaticQueue_t *q) {
  *q = (StaticQueue_t){.data=data, .capacity=cap, .size=size}; return q;
}
int xQueueReset(QueueHandle_t q) { assert(q); q->head=q->count=0; return pdTRUE; }
int xQueueSend(QueueHandle_t q, const void *item, int ticks) {
  (void)ticks; assert(q);
  if (q->count == q->capacity) return 0;
  memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, item, q->size);
  ++q->count; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *item, int ticks) {
  (void)ticks; assert(q);
  if (!q->count) return 0;
  memcpy(item, q->data + q->head * q->size, q->size);
  q->head=(q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
void vQueueDelete(QueueHandle_t q) { assert(q); }

static int baseline;
static int16_t pcm[320] = {123}, out[320];
static bool speech;
static void recover(void) {
  pet_p4_afe_reset_stream();
  assert(pet_p4_afe_preparing());
  assert(pet_p4_afe_prepare());
  assert(!pet_p4_afe_preparing());
  assert(strcmp(pet_p4_afe_failure(), "none") == 0);
  assert(allocations == baseline);
}
static void one_frame(void) {
  assert(pet_p4_afe_feed(pcm));
  assert(pet_p4_afe_take(out, &speech));
  assert(speech && out[0] == 123);
}
int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "cadence") == 0) {
    test_chunk_samples = atoi(argv[2]);
    assert(test_chunk_samples == 256 || test_chunk_samples == 512);
    assert(pet_p4_afe_init());
    size_t read_samples = 0;
    for (unsigned frame = 0; frame < 160; ++frame) {
      for (unsigned i = 0; i < 320; ++i) pcm[i] = (frame * 320 + i) % 30000;
      pet_p4_afe_reference(pcm, 320);
      assert(pet_p4_afe_feed(pcm));
      while (pet_p4_afe_take(out, &speech)) {
        for (unsigned i = 0; i < 320; ++i) assert(out[i] == (int16_t)(read_samples++ % 30000));
      }
    }
    assert(read_samples == 160 * 320);
    assert(pet_p4_afe_metrics().frames == 160);
    puts("AEC chunk cadence: every sample preserved across USB frames");
    return 0;
  }
  if (argc > 1) {
    fail_after = atoi(argv[1]);
    assert(!pet_p4_afe_init());
    assert(allocations == 0 && !pet_p4_afe_ready());
    assert(strcmp(pet_p4_afe_failure(), "init_failed") == 0);
    pet_p4_afe_reset_stream();
    assert(pet_p4_afe_prepare());
  } else assert(pet_p4_afe_init());
  baseline = allocations;
  one_frame();
  unsigned retired = atomic_load(&generation);
  recover();
  fail(AFE_REFERENCE_OVERFLOW, retired);
  assert(pet_p4_afe_ready() && strcmp(pet_p4_afe_failure(), "none") == 0);
  for (int i=0; i<50; ++i) { recover(); one_frame(); }

  /* A transport stall overflows reference; same generation cannot silently
   * resume raw audio. A new entry recovers without replacing bulk buffers. */
  for (int i=0; i<27; ++i) pet_p4_afe_reference(pcm, 320);
  assert(!pet_p4_afe_ready());
  assert(strcmp(pet_p4_afe_failure(), "reference_overflow") == 0);
  int attempts = dsp_creates;
  for (int i=0; i<50; ++i) assert(!pet_p4_afe_feed(pcm));
  assert(dsp_creates == attempts && !pet_p4_afe_take(out, &speech));
  assert(!pet_p4_afe_init() && allocations == baseline);
  recover(); one_frame();

  for (int i=0; i<4; ++i) assert(pet_p4_afe_feed(pcm));
  assert(!pet_p4_afe_feed(pcm));
  assert(strcmp(pet_p4_afe_failure(), "processed_queue_overflow") == 0);
  recover(); assert(!pet_p4_afe_take(out, &speech)); one_frame();

  process_us=70000;
  one_frame(); one_frame();
  assert(!pet_p4_afe_feed(pcm));
  assert(strcmp(pet_p4_afe_failure(), "dsp_too_slow") == 0);
  pet_p4_afe_metrics_t slow = pet_p4_afe_metrics();
  assert(slow.last_us == 70000 && slow.max_us == 70000);
  assert(slow.aec_us == 70000 && slow.ns_us == 0 && slow.vad_us == 0);
  /* Fault diagnostics survive revoking ready, until a new timeline starts. */
  assert(slow.frames >= 3);
  recover();
  pet_p4_afe_metrics_t reset = pet_p4_afe_metrics();
  assert(reset.frames == 0 && reset.last_us == 0 && reset.max_us == 0);
  assert(reset.reference_samples == 0);
  recover(); one_frame(); /* slow streak reset, not immediately latched again */
  process_us=0;

  /* Both reserve allocations and each recreated DSP handle may fail. */
  for (int i=0; i<5; ++i) {
    pet_p4_afe_reset_stream(); fail_after=i;
    assert(!pet_p4_afe_prepare());
    assert(strcmp(pet_p4_afe_failure(), "stream_reset_failed") == 0);
    assert(!aec && !ns && !vad);
    assert(allocations == baseline - 3); /* no partial handles/reserve leak */
    assert(!pet_p4_afe_preparing());
    attempts=dsp_creates;
    assert(!pet_p4_afe_prepare() && dsp_creates == attempts);
    recover(); one_frame();
  }
  assert(pet_p4_afe_feed(pcm));
  pet_p4_afe_reset_stream();
  assert(!pet_p4_afe_take(out, &speech));
  assert(pet_p4_afe_prepare()); one_frame();
  puts("AEC lifecycle: allocation failures, bounded recovery, stale queues and slow streaks passed");
  return 0;
}
