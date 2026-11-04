#include "pet_p4_trusted_services.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* Fault-injected trusted boundary, not an emulated peripheral driver. The
 * separate adapter test links the real broker and SDK-shaped providers. */
typedef enum { HOOK_NONE, HOOK_AUDIO, HOOK_ACQUIRE, HOOK_CURRENT, HOOK_CLOCK_START,
  HOOK_SAMPLE, HOOK_CLOCK_END, HOOK_FINISH, HOOK_REVOKE } hook_t;
typedef struct {
  pet_resource_broker_t broker;
  pet_resource_owner_t owner;
  pet_resource_lease_t held;
  pet_resource_result_t admission;
  pet_resource_finish_t completion;
  int64_t times[2];
  unsigned clocks, samples, acquired, finishes, revokes, checks, audio_checks;
  unsigned audio_at, stale_at;
  bool live, occupied, inside_provider, revoke_in_provider, busy_in_provider;
  pet_trusted_sample_result_t sample_result;
  pet_trusted_service_backend_t backend;
  pet_trusted_service_authority_t *authority;
  hook_t hook;
  bool hooked, cancel_at_hook, mutate_at_hook, mutate_backend, foreign_ticket;
} fixture_t;
static void callback(fixture_t *f, hook_t at) {
  if (f->hook != at || f->hooked) return;
  f->hooked = true;
  pet_trusted_service_snapshot_t out, before;
  pet_trusted_service_diagnostic_t d;
  memset(&out, 0x5a, sizeof(out)); before = out;
  assert(f->authority->in_flight);
  uint16_t budget = f->authority->remaining_reads;
  unsigned acquired = f->acquired, clocks = f->clocks, samples = f->samples, finishes = f->finishes;
  pet_trusted_service_result_t expected = f->authority->cancelled ? PET_TRUSTED_CANCELLED : PET_TRUSTED_BUSY;
  assert(pet_trusted_service_read(f->authority, 2, &f->backend, &out, &d) == expected);
  assert(d.result == expected && !d.snapshot_published && !memcmp(&out, &before, sizeof(out)));
  assert(!pet_trusted_service_authority_init(f->authority, f->owner, PET_TRUSTED_SERVICE_ALL_MASK, 16));
  assert(f->authority->remaining_reads == budget);
  assert(f->acquired == acquired && f->clocks == clocks && f->samples == samples && f->finishes == finishes);
  if (f->cancel_at_hook) f->authority->cancelled = true;
  if (f->mutate_at_hook) f->authority->owner.generation++;
  if (f->mutate_backend) f->backend.finish = NULL;
}
static int64_t clock_us(void *context) {
  fixture_t *f = context;
  assert(f->occupied);
  unsigned index = f->clocks++ % 2;
  callback(f, index ? HOOK_CLOCK_END : HOOK_CLOCK_START);
  return f->times[index];
}
static pet_trusted_sample_result_t sample(void *context, pet_trusted_joystick_t *out) {
  fixture_t *f = context;
  assert(f->occupied);
  f->inside_provider = true; f->samples++;
  callback(f, HOOK_SAMPLE);
  if (f->revoke_in_provider) { f->live = false; assert(f->occupied); }
  if (f->busy_in_provider) f->audio_at = f->audio_checks + 1;
  *out = (pet_trusted_joystick_t){true, 2000, 2100, 2200, 2300, 10, 4000, 20, 4020};
  f->inside_provider = false;
  return f->sample_result;
}
static bool busy(void *context) {
  fixture_t *f = context;
  f->audio_checks++;
  callback(f, HOOK_AUDIO);
  return f->audio_at && f->audio_checks >= f->audio_at;
}
static pet_resource_result_t acquire(void *context, pet_resource_owner_t owner,
    pet_resource_kind_t kind, uint32_t units, pet_resource_lease_t *out) {
  fixture_t *f = context;
  f->acquired++;
  assert(owner.broker == &f->broker && owner.generation == f->owner.generation);
  assert(kind == PET_RESOURCE_PERIPHERAL && units == 1 && !out->serial);
  if (f->admission != PET_RESOURCE_OK) return f->admission;
  assert(!f->occupied);
  f->held = (pet_resource_lease_t){owner, f->acquired, units, kind, 0};
  *out = f->held; f->occupied = true;
  if (f->foreign_ticket) out->owner.generation++;
  callback(f, HOOK_ACQUIRE);
  return PET_RESOURCE_OK;
}
static bool current(void *context, pet_resource_lease_t lease) {
  fixture_t *f = context;
  f->checks++;
  assert(f->occupied && lease.serial == f->held.serial);
  if (f->stale_at && f->checks >= f->stale_at) f->live = false;
  callback(f, HOOK_CURRENT);
  return f->live;
}
static void revoke(void *context, pet_resource_owner_t owner) {
  fixture_t *f = context;
  assert(owner.generation == f->owner.generation);
  f->revokes++; f->live = false;
  callback(f, HOOK_REVOKE);
}
static pet_resource_finish_t finish(void *context, pet_resource_lease_t lease) {
  fixture_t *f = context;
  assert(f->occupied && !f->inside_provider && lease.serial == f->held.serial);
  f->finishes++; f->occupied = false;
  callback(f, HOOK_FINISH);
  return f->live ? f->completion : PET_RESOURCE_FINISH_REVOKED;
}
static void init(fixture_t *f, pet_trusted_service_authority_t *a) {
  memset(f, 0, sizeof(*f));
  *a = (pet_trusted_service_authority_t){0};
  f->owner = (pet_resource_owner_t){&f->broker, 1, 0};
  f->admission = PET_RESOURCE_OK; f->completion = PET_RESOURCE_FINISH_CURRENT;
  f->times[0] = 100000; f->times[1] = 100050; f->live = true;
  f->sample_result = PET_TRUSTED_SAMPLE_READY;
  f->backend = (pet_trusted_service_backend_t){f, clock_us, sample, busy, acquire, current, revoke, finish};
  f->authority = a;
  assert(pet_trusted_service_authority_init(a, f->owner, PET_TRUSTED_SERVICE_ALL_MASK, 16));
}
static void rejected(fixture_t *f, pet_trusted_service_authority_t *a,
    uint32_t service, pet_trusted_service_result_t expected) {
  pet_trusted_service_snapshot_t out, before;
  memset(&out, 0xa5, sizeof(out)); before = out;
  pet_trusted_service_diagnostic_t diagnostic;
  assert(pet_trusted_service_read(a, service, &f->backend, &out, &diagnostic) == expected);
  assert(diagnostic.result == expected && !diagnostic.snapshot_published);
  assert(!memcmp(&before, &out, sizeof(out)) && !f->occupied);
}
int main(void) {
  fixture_t f;
  pet_trusted_service_authority_t a;
  pet_trusted_service_snapshot_t out;
  pet_trusted_service_diagnostic_t d;
  assert(!pet_trusted_service_descriptor(0) && !pet_trusted_service_descriptor(3));
  assert(!pet_trusted_service_descriptor(UINT32_MAX));
  assert(!strcmp(pet_trusted_service_descriptor(1)->name, "clock.monotonic"));
  assert(!strcmp(pet_trusted_service_descriptor(2)->name, "input.joystick"));
  init(&f, &a);
  pet_trusted_service_authority_t original = a, copied = a;
  pet_trusted_service_snapshot_t copied_out, copied_out_before;
  pet_trusted_service_diagnostic_t copied_d, copied_d_before;
  memset(&copied_out, 0x5a, sizeof(copied_out)); copied_out_before = copied_out;
  memset(&copied_d, 0xa5, sizeof(copied_d)); copied_d_before = copied_d;
  for (unsigned service = 1; service <= 2; ++service) {
    assert(pet_trusted_service_read(&copied, service, &f.backend, &copied_out, &copied_d) == PET_TRUSTED_INVALID);
    assert(!memcmp(&a, &original, sizeof(a)) && !memcmp(&copied, &original, sizeof(copied)));
    assert(!memcmp(&copied_out, &copied_out_before, sizeof(copied_out)));
    assert(!memcmp(&copied_d, &copied_d_before, sizeof(copied_d)));
    assert(!f.audio_checks && !f.acquired && !f.samples && !f.clocks && !f.revokes && f.live);
    assert(!pet_trusted_service_authority_init(&copied, f.owner, 3, 16));
  }
  assert(!pet_trusted_service_authority_init(&a, f.owner, 4, 1));
  assert(!pet_trusted_service_authority_init(&a, f.owner, 1, 0));
  assert(!pet_trusted_service_authority_init(&a, f.owner, 1, 17));
  assert(pet_trusted_service_read(&a, 1, &f.backend, &out, &d) == PET_TRUSTED_OK);
  assert(out.service == 1 && out.version == 1 && out.value.monotonic_us == 100000);
  assert(d.elapsed_us == 50 && d.provider_completed && d.snapshot_published);
  assert(!f.samples && f.finishes == 1 && a.remaining_reads == 15);
  assert(pet_trusted_service_read(&a, 2, &f.backend, &out, &d) == PET_TRUSTED_OK);
  assert(out.value.joystick.current_x == 2200 && out.value.joystick.center_y == 2100);
  assert(f.samples == 1 && f.finishes == 2 && a.remaining_reads == 14);
  puts("PASS fixed real-read catalog and bounded snapshot output");

  init(&f, &a); a.granted_mask = PET_TRUSTED_CLOCK_MASK;
  rejected(&f, &a, 2, PET_TRUSTED_DENIED);
  rejected(&f, &a, 65535, PET_TRUSTED_UNKNOWN_SERVICE);
  assert(!f.acquired && !f.samples && !f.clocks && a.remaining_reads == 16);
  for (unsigned i = 0; i < 16; i++)
    assert(pet_trusted_service_read(&a, 1, &f.backend, &out, NULL) == PET_TRUSTED_OK);
  rejected(&f, &a, 1, PET_TRUSTED_CALL_LIMIT);
  assert(f.acquired == 16);
  puts("PASS exact authorization, unknown services and per-invocation read budget");

  const pet_resource_result_t admissions[] = {PET_RESOURCE_STALE, PET_RESOURCE_FORBIDDEN,
    PET_RESOURCE_PROTECTED, PET_RESOURCE_UNAVAILABLE, PET_RESOURCE_BUSY, PET_RESOURCE_EXHAUSTED,
    PET_RESOURCE_INVALID};
  const pet_trusted_service_result_t expected[] = {PET_TRUSTED_CANCELLED, PET_TRUSTED_DENIED,
    PET_TRUSTED_AUDIO_PROTECTED, PET_TRUSTED_UNAVAILABLE, PET_TRUSTED_BUSY, PET_TRUSTED_BUSY,
    PET_TRUSTED_INVALID};
  for (unsigned i = 0; i < sizeof(admissions) / sizeof(admissions[0]); i++) {
    init(&f, &a); f.admission = admissions[i];
    rejected(&f, &a, 2, expected[i]);
    assert(!f.samples && !f.clocks && !f.finishes && a.remaining_reads == 15);
  }
  puts("PASS resource admission rejection performs no provider call");

  for (unsigned phase = 1; phase <= 5; phase++) {
    init(&f, &a); f.audio_at = phase;
    rejected(&f, &a, 2, PET_TRUSTED_AUDIO_PROTECTED);
    assert(a.cancelled && f.revokes && f.finishes == (phase > 1));
    assert(f.samples == (phase >= 4));
    f.audio_at = 0;
    rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
  }
  init(&f, &a); f.revoke_in_provider = true;
  rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
  assert(f.samples == 1 && f.finishes == 1 && a.cancelled);
  init(&f, &a); f.stale_at = 1;
  rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
  assert(!f.samples && f.finishes == 1);
  init(&f, &a); f.completion = PET_RESOURCE_FINISH_STALE;
  rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
  assert(a.cancelled && f.revokes && !f.live);
  puts("PASS stale owner and audio cancellation retain occupancy until provider returns");

  init(&f, &a); f.times[1] = f.times[0] + PET_TRUSTED_SERVICE_MAX_US;
  assert(pet_trusted_service_read(&a, 2, &f.backend, &out, &d) == PET_TRUSTED_OK);
  init(&f, &a); f.times[1] = f.times[0] + PET_TRUSTED_SERVICE_MAX_US + 1;
  rejected(&f, &a, 2, PET_TRUSTED_TIMEOUT);
  assert(a.cancelled && f.finishes == 1 && f.samples == 1);
  init(&f, &a); f.times[0] = -1;
  rejected(&f, &a, 2, PET_TRUSTED_CLOCK_ERROR);
  assert(a.cancelled && !f.samples && f.finishes == 1);
  init(&f, &a); f.times[1] = 1;
  rejected(&f, &a, 2, PET_TRUSTED_CLOCK_ERROR);
  init(&f, &a); f.times[0] = INT64_MAX - 50; f.times[1] = INT64_MAX;
  assert(pet_trusted_service_read(&a, 1, &f.backend, &out, &d) == PET_TRUSTED_OK);
  assert(d.elapsed_us == 50 && out.sampled_us == (uint64_t)INT64_MAX - 50);
  puts("PASS deadline boundaries and invalid monotonic clock fail closed");

  init(&f, &a); f.sample_result = PET_TRUSTED_SAMPLE_NOT_READY;
  rejected(&f, &a, 2, PET_TRUSTED_NOT_READY);
  assert(!a.cancelled && f.finishes == 1 && f.samples == 1);
  init(&f, &a); f.sample_result = PET_TRUSTED_SAMPLE_ERROR;
  rejected(&f, &a, 2, PET_TRUSTED_PROVIDER_ERROR);
  init(&f, &a); f.sample_result = (pet_trusted_sample_result_t)300;
  rejected(&f, &a, 2, PET_TRUSTED_PROVIDER_ERROR);
  puts("PASS not-ready/provider-failure outcomes without automatic resubmission");

  for (hook_t hook = HOOK_AUDIO; hook <= HOOK_FINISH; hook++) {
    init(&f, &a); f.hook = hook;
    assert(pet_trusted_service_read(&a, 2, &f.backend, &out, &d) == PET_TRUSTED_OK);
    assert(f.hooked && !a.in_flight && !a.cancelled && a.remaining_reads == 15);
    assert(f.acquired == 1 && f.samples == 1 && f.finishes == 1);
  }
  init(&f, &a); f.hook = HOOK_REVOKE; f.audio_at = 1;
  rejected(&f, &a, 2, PET_TRUSTED_AUDIO_PROTECTED);
  assert(f.hooked && !a.in_flight && a.cancelled && !f.finishes);
  puts("PASS every native callback rejects recursive read and init without consuming budget or occupancy");

  for (hook_t hook = HOOK_AUDIO; hook <= HOOK_FINISH; hook++) {
    init(&f, &a); f.hook = hook; f.cancel_at_hook = true;
    rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
    assert(f.hooked && a.cancelled && !a.in_flight && !f.live);
    assert(!pet_trusted_service_authority_init(&a, f.owner, 3, 16));
    unsigned clocks = f.clocks, samples = f.samples;
    rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
    assert(f.clocks == clocks && f.samples == samples);
    init(&f, &a); f.hook = hook; f.mutate_at_hook = true;
    rejected(&f, &a, 2, PET_TRUSTED_CANCELLED);
    assert(f.hooked && a.cancelled && !f.live && !a.in_flight);
  }
  init(&f, &a); f.hook = HOOK_SAMPLE; f.mutate_backend = true;
  assert(pet_trusted_service_read(&a, 2, &f.backend, &out, &d) == PET_TRUSTED_OK);
  assert(f.finishes == 1 && !f.occupied);
  puts("PASS callback cancellation/identity changes cannot publish or revive and exact frozen backend finishes");

  /* Check complete and partial overlap before diagnostic initialization. These
   * are aligned backing stores, not invalid pointers or out-of-bounds writes. */
  union { max_align_t align; unsigned char bytes[512]; } shared, saved;
  for (unsigned left = 0; left < 4; left++) for (unsigned right = left + 1; right < 4; right++) {
    for (unsigned partial = 0; partial < 2; partial++) {
      init(&f, &a);
      memset(&shared, 0xa5, sizeof(shared)); saved = shared;
      void *p[4] = {&a, &f.backend, &out, &d};
      p[left] = shared.bytes;
      p[right] = shared.bytes + (partial ? _Alignof(max_align_t) : 0);
      assert(pet_trusted_service_read(p[0], 2, p[1], p[2], p[3]) == PET_TRUSTED_INVALID);
      assert(!memcmp(&shared, &saved, sizeof(shared)));
      assert(!f.acquired && !f.clocks && !f.samples && !f.audio_checks);
      assert(a.remaining_reads == 16 && !a.in_flight);
    }
  }
  init(&f, &a);
  assert(!pet_trusted_service_authority_init(&a, f.owner, 3, 16));
  assert(pet_trusted_service_read(&a, 2, &f.backend, &out, &d) == PET_TRUSTED_OK);
  assert(!pet_trusted_service_authority_init(&a, f.owner, 3, 16) && a.remaining_reads == 15);
  puts("PASS all public object overlap pairs preserve every byte and init never resets an existing invocation");

  init(&f, &a); f.foreign_ticket = true;
  memset(&out, 0xa5, sizeof(out)); pet_trusted_service_snapshot_t unchanged = out;
  assert(pet_trusted_service_read(&a, 2, &f.backend, &out, &d) == PET_TRUSTED_INVALID);
  assert(a.cancelled && !a.in_flight && !f.live && f.occupied);
  assert(!f.finishes && !f.clocks && !f.samples && !memcmp(&out, &unchanged, sizeof(out)));
  /* Only the fixture's native owner can now release the genuine retained ticket. */
  assert(finish(&f, f.held) == PET_RESOURCE_FINISH_REVOKED && !f.occupied);
  puts("PASS malformed admission never finishes foreign work or publishes a snapshot");
  return 0;
}
