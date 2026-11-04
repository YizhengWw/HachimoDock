/* Same-source native sanitizer / import-free WASM loader+lifecycle tests. */
#include "pet_p4_program_runtime.h"
#include <limits.h>
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)

static void bytes_copy(void *target, const void *source, size_t n) {
  unsigned char *a = target; const unsigned char *b = source;
  for (size_t i = 0; i < n; i++) a[i] = b[i];
}
static bool bytes_equal(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return false;
  return true;
}
static bool all_zero(const void *a, size_t n) {
  const unsigned char *x = a;
  for (size_t i = 0; i < n; i++) if (x[i]) return false;
  return true;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8U));
}
static void checksum(uint8_t *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < n; i++) {
    if (i >= 20 && i <= 23) continue;
    crc ^= p[i];
    for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ ((0U - (crc & 1U)) & UINT32_C(0xedb88320));
  }
  put32(p + 20, ~crc);
}
static size_t encode(uint8_t *p, const pet_program_insn_t *code, uint16_t count,
    uint16_t fuel, uint8_t cells) {
  for (size_t i = 0; i < PET_PROGRAM_IMAGE_MAX_BYTES; i++) p[i] = 0;
  p[0] = 'P'; p[1] = '4'; p[2] = 'P'; p[3] = '1'; p[4] = 1;
  put16(p + 6, 24); put16(p + 8, count); put16(p + 10, fuel); p[12] = cells;
  put32(p + 16, 42);
  for (unsigned i = 0; i < count; i++) {
    uint8_t *insn = p + 24 + i * 8U;
    insn[0] = code[i].op; insn[1] = code[i].a; insn[2] = code[i].b; insn[3] = code[i].c;
    put32(insn + 4, (uint32_t)code[i].imm);
  }
  size_t n = 24 + (size_t)count * 8U; checksum(p, n); return n;
}
static const pet_program_insn_t accumulator[] = {
  {PET_OP_CONST,0,0,0,0}, {PET_OP_LOAD,1,0,0,0}, {PET_OP_EVENT,2,0,0,0},
  {PET_OP_ADD,1,1,2,0}, {PET_OP_STORE,0,1,0,0}, {PET_OP_OUTPUT,0,1,0,0}, {PET_OP_HALT,0,0,0,0}
};
static pet_program_image_t image;
static uint8_t wire[PET_PROGRAM_IMAGE_MAX_BYTES];
static pet_program_runtime_t runtime, before;
typedef struct {
  uint64_t generation;
  uint32_t calls, deny_at;
  bool live, reenter;
  pet_program_runtime_t *r;
  unsigned denied_reentries;
} guard_t;
static bool current(void *context, uint64_t generation) {
  guard_t *g = context; g->calls++;
  if (g->reenter) {
    pet_program_event_t e = {1,{100,0,0,0}};
    pet_program_report_t report;
    pet_program_snapshot_t snapshot;
    if (pet_program_runtime_pump(g->r, generation) == PET_RUNTIME_BUSY
        && pet_program_runtime_enqueue(g->r,generation,99,&e) == PET_RUNTIME_BUSY
        && pet_program_runtime_cancel(g->r,generation) == PET_RUNTIME_BUSY
        && pet_program_runtime_take_report(g->r,generation,&report) == PET_RUNTIME_BUSY
        && pet_program_runtime_snapshot(g->r,generation,&snapshot) == PET_RUNTIME_BUSY
        && pet_program_runtime_start(g->r,&image,generation+1,current,g,1,NULL) == PET_RUNTIME_BUSY)
      g->denied_reentries++;
  }
  return g->live && generation == g->generation && (!g->deny_at || g->calls < g->deny_at);
}
static int setup(guard_t *g, uint16_t quantum) {
  size_t n = encode(wire, accumulator, 7, 64, 1);
  CHECK(pet_program_image_decode(&image, wire, n) == PET_PROGRAM_IMAGE_OK);
  pet_program_runtime_init(&runtime);
  *g = (guard_t){1,0,0,true,false,&runtime,0};
  CHECK(pet_program_runtime_start(&runtime,&image,1,current,g,quantum,NULL) == PET_RUNTIME_OK);
  return 0;
}
static pet_program_runtime_result_t complete(void) {
  for (unsigned i = 0; i < 5000; i++) {
    pet_program_runtime_result_t result = pet_program_runtime_pump(&runtime, runtime.generation);
    if (result != PET_RUNTIME_YIELDED) return result;
  }
  return PET_RUNTIME_INVALID;
}
static int test_loader_valid(void) {
  /* Golden header+HALT generated independently with Python zlib.crc32. */
  static const uint8_t golden[] = {0x50,0x34,0x50,0x31,1,0,24,0,1,0,1,0,0,0,0,0,
    42,0,0,0,0x7c,0x05,0x14,0xbf,0,0,0,0,0,0,0,0};
  CHECK(pet_program_image_decode(&image,golden,sizeof(golden)) == PET_PROGRAM_IMAGE_OK);
  uint8_t unaligned[sizeof(golden)+1];
  bytes_copy(unaligned+1,golden,sizeof(golden));
  CHECK(pet_program_image_decode(&image,unaligned+1,sizeof(golden)) == PET_PROGRAM_IMAGE_OK);
  size_t n = encode(wire, accumulator, 7, 64, 1);
  CHECK(pet_program_image_decode(&image, wire, n) == PET_PROGRAM_IMAGE_OK);
  CHECK(image.valid && image.schema == 42 && image.count == 7 && image.fuel == 64 && image.cells == 1);
  CHECK(bytes_equal(image.code, accumulator, sizeof(accumulator)));
  CHECK(all_zero(image.code + 7, sizeof(image.code) - sizeof(accumulator)));
  pet_program_t p;
  CHECK(pet_program_image_program(&image, &p) && p.code == image.code && !p.intent_limit);
  wire[24] = 255; CHECK(image.code[0].op == PET_OP_CONST); /* owned, not wire-backed */
  pet_program_insn_t extremes[] = {{PET_OP_CONST,0,0,0,INT32_MIN}, {PET_OP_CONST,1,0,0,INT32_MAX},
    {PET_OP_CONST,2,0,0,-1}, {PET_OP_HALT,0,0,0,0}};
  n = encode(wire,extremes,4,4,0);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_OK);
  CHECK(image.code[0].imm == INT32_MIN && image.code[1].imm == INT32_MAX && image.code[2].imm == -1);
  pet_program_insn_t maximum[PET_PROGRAM_CODE] = {{0}};
  n = encode(wire,maximum,PET_PROGRAM_CODE,PET_PROGRAM_FUEL,PET_PROGRAM_CELLS);
  CHECK(n == PET_PROGRAM_IMAGE_MAX_BYTES);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_OK);
  CHECK(image.count == PET_PROGRAM_CODE && image.fuel == PET_PROGRAM_FUEL && image.cells == PET_PROGRAM_CELLS);
  return 0;
}
static int test_loader_errors(void) {
  size_t n = encode(wire, accumulator, 7, 64, 1);
  CHECK(pet_program_image_decode(&image, wire, n) == PET_PROGRAM_IMAGE_OK);
  pet_program_image_t saved = image;
  CHECK(pet_program_image_decode(&image,NULL,n) == PET_PROGRAM_IMAGE_INVALID_ARGUMENT);
  CHECK(bytes_equal(&saved,&image,sizeof(image)));
  CHECK(pet_program_image_decode(&image,(uint8_t *)&image,n) == PET_PROGRAM_IMAGE_INVALID_ARGUMENT);
  CHECK(bytes_equal(&saved,&image,sizeof(image)));
  for (size_t length = 0; length < n; length++) {
    CHECK(pet_program_image_decode(&image,wire,length) != PET_PROGRAM_IMAGE_OK);
    CHECK(all_zero(&image,sizeof(image)));
  }
  CHECK(pet_program_image_decode(&image,wire,n+1) == PET_PROGRAM_IMAGE_BAD_LENGTH);
  CHECK(pet_program_image_decode(&image,wire,SIZE_MAX) != PET_PROGRAM_IMAGE_OK);
  for (unsigned field = 0; field < 24; field++) {
    n = encode(wire, accumulator, 7, 64, 1); wire[field] ^= 128;
    CHECK(pet_program_image_decode(&image,wire,n) != PET_PROGRAM_IMAGE_OK);
    CHECK(all_zero(&image,sizeof(image)));
  }
  for (size_t field = 24; field < n; field++) {
    n = encode(wire, accumulator, 7, 64, 1); wire[field] ^= 1;
    CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_BAD_CHECKSUM);
  }
  const unsigned reserved[] = {4,5,6,7,13,14,15};
  for (unsigned i = 0; i < sizeof(reserved)/sizeof(reserved[0]); i++) {
    n = encode(wire, accumulator, 7, 64, 1); wire[reserved[i]] ^= 4; checksum(wire,n);
    CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_BAD_HEADER);
  }
  n = encode(wire, accumulator, 7, 64, 1); put32(wire+16,0); checksum(wire,n);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_BAD_HEADER);
  n = encode(wire, accumulator, 7, 0, 1);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_BAD_CODE);
  n = encode(wire, accumulator, 7, 64, 65);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_BAD_CODE);
  pet_program_insn_t bad[] = {{PET_OP_HALT,0,0,0,0},{PET_OP_INTENT,0,0,0,0}};
  n = encode(wire,bad,2,64,1);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_EFFECT_FORBIDDEN);
  bad[1] = (pet_program_insn_t){255,0,0,0,0}; n = encode(wire,bad,2,64,1);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_BAD_CODE);
  CHECK(all_zero(&image,sizeof(image)));
  return 0;
}
static int test_queue_transaction(void) {
  guard_t g; CHECK(!setup(&g,2));
  pet_program_event_t e = {1,{3,99,99,99}};
  for (uint64_t i = 1; i <= PET_PROGRAM_QUEUE; i++)
    CHECK(pet_program_runtime_enqueue(&runtime,1,i,&e) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_enqueue(&runtime,1,5,&e) == PET_RUNTIME_QUEUE_FULL);
  CHECK(runtime.last_sequence == 4 && runtime.queue[0].words[1] == 0);
  CHECK(pet_program_runtime_enqueue(&runtime,1,3,&e) == PET_RUNTIME_DUPLICATE);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
  CHECK(runtime.inflight == 1 && runtime.workspace.used_fuel == 0 && runtime.state.revision == 0);
  CHECK(pet_program_runtime_enqueue(&runtime,1,5,&e) == PET_RUNTIME_OK);
  for (unsigned i = 0; i < 4; i++) {
    uint16_t old_fuel = runtime.workspace.used_fuel;
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
    CHECK(runtime.workspace.used_fuel - old_fuel <= 2 && runtime.state.revision == 0);
  }
  CHECK(runtime.workspace.result == PET_PROGRAM_READY && runtime.state.cells[0] == 0);
  pet_program_snapshot_t snapshot;
  CHECK(pet_program_runtime_snapshot(&runtime,1,&snapshot) == PET_RUNTIME_OK);
  CHECK(snapshot.values[0] == 0);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_COMPLETE);
  CHECK(runtime.state.revision == 1 && runtime.state.cells[0] == 3);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_REPORT_PENDING);
  pet_program_report_t r;
  CHECK(pet_program_runtime_take_report(&runtime,1,&r) == PET_RUNTIME_OK);
  CHECK(r.sequence == 1 && r.result == PET_PROGRAM_COMMITTED && r.used_fuel == 7 && r.revision == 1);
  CHECK(pet_program_runtime_take_report(&runtime,1,&r) == PET_RUNTIME_IDLE);
  for (uint64_t i = 2; i <= 5; i++) {
    CHECK(complete() == PET_RUNTIME_COMPLETE);
    CHECK(pet_program_runtime_take_report(&runtime,1,&r) == PET_RUNTIME_OK);
    CHECK(r.sequence == i && r.revision == i && r.result == PET_PROGRAM_COMMITTED);
  }
  CHECK(runtime.state.cells[0] == 15 && pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_IDLE);
  CHECK(pet_program_runtime_enqueue(&runtime,1,5,&e) == PET_RUNTIME_DUPLICATE);
  return 0;
}
static int test_all_quanta_and_owned_code(void) {
  for (uint16_t q = 1; q <= PET_PROGRAM_QUANTUM; q++) {
    guard_t g; CHECK(!setup(&g,q));
    image.code[0].op = 255; /* Runtime owns its activation copy. */
    pet_program_event_t e = {1,{9,0,0,0}};
    CHECK(pet_program_runtime_enqueue(&runtime,1,UINT64_MAX,&e) == PET_RUNTIME_OK);
    unsigned phases = 0;
    for (;;) {
      uint16_t old = runtime.workspace.used_fuel;
      pet_program_runtime_result_t result = pet_program_runtime_pump(&runtime,1); phases++;
      CHECK(runtime.workspace.used_fuel - old <= q);
      if (result == PET_RUNTIME_COMPLETE) break;
      CHECK(result == PET_RUNTIME_YIELDED && phases < 20);
    }
    CHECK(phases == 2U + (7U + q - 1U) / q && runtime.state.outputs[0] == 9);
    CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_DUPLICATE);
  }
  return 0;
}
static int test_cancellation_and_stale(void) {
  for (unsigned phase = 0; phase < 5; phase++) {
    guard_t g; CHECK(!setup(&g,2)); pet_program_event_t e = {1,{9,0,0,0}};
    CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_OK);
    CHECK(pet_program_runtime_enqueue(&runtime,1,2,&e) == PET_RUNTIME_OK);
    for (unsigned i = 0; i < phase; i++) CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
    g.live = false;
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_DENIED);
    CHECK(!runtime.active && runtime.state.revision == 0 && runtime.cancelled_events == 2);
    g.live = true;
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_STOPPED);
    CHECK(pet_program_runtime_enqueue(&runtime,1,3,&e) == PET_RUNTIME_STOPPED);
    CHECK(all_zero(&runtime.workspace,sizeof(runtime.workspace)));
    pet_program_report_t r;
    CHECK(pet_program_runtime_take_report(&runtime,1,&r) == (phase ? PET_RUNTIME_OK : PET_RUNTIME_IDLE));
    if (phase) CHECK(r.sequence == 1 && r.result == PET_PROGRAM_CANCELLED);
    CHECK(pet_program_runtime_start(&runtime,&image,1,current,&g,2,NULL) == PET_RUNTIME_STALE);
    g.generation = 2;
    CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,2,NULL) == PET_RUNTIME_OK);
    bytes_copy(&before,&runtime,sizeof(runtime));
    CHECK(pet_program_runtime_cancel(&runtime,1) == PET_RUNTIME_STALE);
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_STALE);
    CHECK(pet_program_runtime_enqueue(&runtime,1,100,&e) == PET_RUNTIME_STALE);
    CHECK(bytes_equal(&before,&runtime,sizeof(runtime)));
  }
  return 0;
}
static int test_instruction_revocation_and_reentry(void) {
  guard_t g; CHECK(!setup(&g,64)); pet_program_event_t e = {1,{9,0,0,0}};
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
  g.deny_at = g.calls + 4; /* Pump precheck, two instructions, then deny. */
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_COMPLETE);
  CHECK(runtime.report.result == PET_PROGRAM_CANCELLED && runtime.report.used_fuel == 2);
  CHECK(!runtime.active && runtime.cancelled_events == 1 && runtime.state.revision == 0);
  CHECK(!setup(&g,2)); g.reenter = true;
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_OK);
  CHECK(complete() == PET_RUNTIME_COMPLETE);
  CHECK(g.denied_reentries == g.calls - 1 && runtime.state.outputs[0] == 9);
  return 0;
}
static int test_trap_and_no_replay(void) {
  guard_t g; CHECK(!setup(&g,2)); CHECK(pet_program_runtime_cancel(&runtime,1) == PET_RUNTIME_OK);
  pet_program_insn_t loop[] = {{PET_OP_CONST,0,0,0,0},{PET_OP_CONST,1,0,0,99},
    {PET_OP_STORE,0,1,0,0},{PET_OP_JUMP,0,0,0,3}};
  size_t n = encode(wire,loop,4,9,1);
  CHECK(pet_program_image_decode(&image,wire,n) == PET_PROGRAM_IMAGE_OK);
  g.generation = 2; CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,2,NULL) == PET_RUNTIME_OK);
  pet_program_event_t e = {0,{0,0,0,0}};
  CHECK(pet_program_runtime_enqueue(&runtime,2,1,&e) == PET_RUNTIME_OK);
  CHECK(complete() == PET_RUNTIME_COMPLETE);
  CHECK(runtime.state.cells[0] == 0 && runtime.state.revision == 0 && runtime.report.used_fuel == 9);
  CHECK(runtime.report.result == PET_PROGRAM_FUEL_EXHAUSTED && runtime.active);
  pet_program_report_t r;
  CHECK(pet_program_runtime_take_report(&runtime,2,&r) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_pump(&runtime,2) == PET_RUNTIME_IDLE);
  CHECK(pet_program_runtime_enqueue(&runtime,2,1,&e) == PET_RUNTIME_DUPLICATE);
  CHECK(pet_program_runtime_enqueue(&runtime,2,2,&e) == PET_RUNTIME_OK);
  CHECK(complete() == PET_RUNTIME_COMPLETE && runtime.report.sequence == 2);
  return 0;
}
static int test_snapshot_restart_and_rollback(void) {
  guard_t g; CHECK(!setup(&g,64)); pet_program_event_t e = {1,{7,0,0,0}};
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_OK && complete() == PET_RUNTIME_COMPLETE);
  CHECK(pet_program_runtime_cancel(&runtime,1) == PET_RUNTIME_OK);
  g.generation = 2;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,NULL) == PET_RUNTIME_BUSY);
  pet_program_report_t report;
  CHECK(pet_program_runtime_take_report(&runtime,1,&report) == PET_RUNTIME_OK);
  CHECK(report.result == PET_PROGRAM_COMMITTED); /* Stop never relabels commit. */
  pet_program_snapshot_t seed;
  CHECK(pet_program_runtime_snapshot(&runtime,1,&seed) == PET_RUNTIME_OK);
  CHECK(seed.values[0] == 7 && seed.revision == 1 && seed.schema == 42);
  bytes_copy(&before,&runtime,sizeof(runtime));
  seed.schema++;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_BAD_STATE);
  CHECK(bytes_equal(&before,&runtime,sizeof(runtime))); seed.schema--;
  seed.cells++;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_BAD_STATE); seed.cells--;
  seed.values[63] = 1;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_BAD_STATE); seed.values[63] = 0;
  seed.schema = 0;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_BAD_STATE); seed.schema = 42;
  g.live = false;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_DENIED);
  CHECK(bytes_equal(&before,&runtime,sizeof(runtime))); g.live = true;
  image.code[0].op = PET_OP_INTENT;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_BAD_IMAGE);
  CHECK(bytes_equal(&before,&runtime,sizeof(runtime))); image.code[0] = accumulator[0];
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,64,&seed) == PET_RUNTIME_OK);
  CHECK(runtime.state.generation == 2 && runtime.state.revision == 1 && runtime.state.cells[0] == 7);
  CHECK(pet_program_runtime_enqueue(&runtime,2,1,&e) == PET_RUNTIME_OK && complete() == PET_RUNTIME_COMPLETE);
  CHECK(runtime.state.cells[0] == 14 && runtime.state.revision == 2);
  return 0;
}
static int test_commit_boundary(void) {
  for (unsigned conflict = 0; conflict < 3; conflict++) {
    guard_t g; CHECK(!setup(&g,64)); pet_program_event_t e = {1,{9,0,0,0}};
    CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_OK);
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
    if (conflict == 0) runtime.state.revision++; /* Trusted concurrent publication. */
    else if (conflict == 1) g.deny_at = g.calls + 2; /* Last commit authority check. */
    else runtime.state.outbox_count = 1; /* Integration must not admit IO. */
    CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_COMPLETE);
    CHECK(runtime.report.result == (conflict == 0 ? PET_PROGRAM_CONFLICT
      : conflict == 1 ? PET_PROGRAM_CANCELLED : PET_PROGRAM_OUTBOX_BUSY));
    CHECK(runtime.state.cells[0] == 0);
  }
  guard_t g; CHECK(!setup(&g,64)); pet_program_event_t e = {1,{9,0,0,0}};
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_YIELDED);
  g.deny_at = g.calls + 3; /* Would deny any post-commit callback. */
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_COMPLETE);
  CHECK(runtime.report.result == PET_PROGRAM_COMMITTED && g.calls + 1 == g.deny_at);
  CHECK(pet_program_runtime_pump(&runtime,1) == PET_RUNTIME_DENIED);
  CHECK(runtime.report.result == PET_PROGRAM_COMMITTED && runtime.state.cells[0] == 9);
  return 0;
}
static int test_argument_boundaries(void) {
  guard_t g; CHECK(!setup(&g,1));
  pet_program_event_t e = {5,{0,0,0,0}};
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&e) == PET_RUNTIME_INVALID); e.count = 0;
  CHECK(pet_program_runtime_enqueue(&runtime,1,0,&e) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&runtime.queue[0]) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_take_report(&runtime,1,&runtime.report) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_snapshot(&runtime,1,(pet_program_snapshot_t *)&runtime.state) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,1,NULL) == PET_RUNTIME_BUSY);
  CHECK(pet_program_runtime_cancel(&runtime,1) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,0,NULL) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,65,NULL) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_start(&runtime,&runtime.image,2,current,&g,1,NULL) == PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_pump(NULL,1) == PET_RUNTIME_INVALID);
  pet_program_runtime_t uninitialized = {0};
  CHECK(pet_program_runtime_pump(&uninitialized,1) == PET_RUNTIME_INVALID);
  return 0;
}
static int test_exhausted_revision_restore(void) {
  guard_t g; CHECK(!setup(&g,2));
  CHECK(pet_program_runtime_cancel(&runtime,1) == PET_RUNTIME_OK);
  pet_program_snapshot_t seed = {.schema=42,.revision=UINT32_MAX-1U,.cells=1,.values={5}};
  g.generation=2;
  CHECK(pet_program_runtime_start(&runtime,&image,2,current,&g,2,&seed) == PET_RUNTIME_OK);
  pet_program_event_t e = {1,{2,0,0,0}};
  CHECK(pet_program_runtime_enqueue(&runtime,2,1,&e) == PET_RUNTIME_OK);
  CHECK(complete() == PET_RUNTIME_COMPLETE);
  pet_program_report_t report;
  CHECK(pet_program_runtime_take_report(&runtime,2,&report) == PET_RUNTIME_OK);
  CHECK(report.result == PET_PROGRAM_COMMITTED && report.revision == UINT32_MAX);
  CHECK(pet_program_runtime_snapshot(&runtime,2,&seed) == PET_RUNTIME_OK);
  CHECK(seed.revision == UINT32_MAX && seed.values[0] == 7 && seed.outputs[0] == 7);
  CHECK(pet_program_runtime_cancel(&runtime,2) == PET_RUNTIME_OK);
  g.generation=3;
  CHECK(pet_program_runtime_start(&runtime,&image,3,current,&g,2,&seed) == PET_RUNTIME_OK);
  pet_program_snapshot_t after;
  CHECK(pet_program_runtime_snapshot(&runtime,3,&after) == PET_RUNTIME_OK);
  CHECK(bytes_equal(&seed,&after,sizeof(seed)));
  CHECK(pet_program_runtime_enqueue(&runtime,3,1,&e) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_pump(&runtime,3) == PET_RUNTIME_COMPLETE);
  CHECK(pet_program_runtime_take_report(&runtime,3,&report) == PET_RUNTIME_OK);
  CHECK(report.result == PET_PROGRAM_OVERFLOW && report.revision == UINT32_MAX && report.used_fuel == 0);
  CHECK(pet_program_runtime_snapshot(&runtime,3,&after) == PET_RUNTIME_OK);
  CHECK(bytes_equal(&seed,&after,sizeof(seed)));
  CHECK(pet_program_runtime_enqueue(&runtime,3,1,&e) == PET_RUNTIME_DUPLICATE);
  CHECK(pet_program_runtime_pump(&runtime,3) == PET_RUNTIME_IDLE);
  CHECK(pet_program_runtime_enqueue(&runtime,3,2,&e) == PET_RUNTIME_OK);
  CHECK(pet_program_runtime_pump(&runtime,3) == PET_RUNTIME_COMPLETE);
  CHECK(runtime.report.result == PET_PROGRAM_OVERFLOW && runtime.state.revision == UINT32_MAX);
  CHECK(pet_program_runtime_snapshot(&runtime,3,&after) == PET_RUNTIME_OK);
  CHECK(bytes_equal(&seed,&after,sizeof(seed)));
  return 0;
}
static uint32_t random_state = UINT32_C(0x13574268);
static uint32_t next_random(void) {
  random_state ^= random_state << 13; random_state ^= random_state >> 17;
  random_state ^= random_state << 5; return random_state;
}
int pet_program_runtime_fuzz(uint32_t trials) {
  for (uint32_t t = 0; t < trials; t++) {
    size_t length = next_random() % (PET_PROGRAM_IMAGE_MAX_BYTES + 1U);
    for (size_t i = 0; i < length; i++) wire[i] = (uint8_t)next_random();
    if (t & 1U) {
      length = encode(wire,accumulator,7,64,1);
      size_t at = next_random() % length; wire[at] ^= (uint8_t)(1U << (next_random() % 8U));
      if (t & 2U) checksum(wire,length);
    }
    pet_program_image_result_t r = pet_program_image_decode(&image,wire,length);
    if (r == PET_PROGRAM_IMAGE_OK) {
      pet_program_t p;
      CHECK(pet_program_image_program(&image,&p) && p.count <= PET_PROGRAM_CODE && p.intent_limit == 0);
      for (unsigned i = 0; i < p.count; i++) CHECK(p.code[i].op != PET_OP_INTENT);
    } else CHECK(all_zero(&image,sizeof(image)));
  }
  return 0;
}
static int test_copied_instance_has_no_authority(void) {
  guard_t g; CHECK(setup(&g,2)==0);
  pet_program_event_t event={1,{9}};
  CHECK(pet_program_runtime_enqueue(&runtime,1,1,&event)==PET_RUNTIME_OK);
  before=runtime;pet_program_runtime_t copy=runtime,copy_before=copy;
  unsigned calls=g.calls;pet_program_report_t report={0},report_before=report;
  pet_program_snapshot_t snapshot={0},snapshot_before=snapshot;
  CHECK(pet_program_runtime_enqueue(&copy,1,2,&event)==PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_pump(&copy,1)==PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_cancel(&copy,1)==PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_take_report(&copy,1,&report)==PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_snapshot(&copy,1,&snapshot)==PET_RUNTIME_INVALID);
  CHECK(pet_program_runtime_start(&copy,&image,2,current,&g,2,NULL)==PET_RUNTIME_INVALID);
  CHECK(g.calls==calls&&bytes_equal(&runtime,&before,sizeof(runtime)));
  CHECK(bytes_equal(&copy,&copy_before,sizeof(copy)));
  CHECK(bytes_equal(&report,&report_before,sizeof(report))&&bytes_equal(&snapshot,&snapshot_before,sizeof(snapshot)));
  CHECK(pet_program_runtime_cancel(&runtime,1)==PET_RUNTIME_OK);
  return 0;
}
int pet_program_runtime_case(unsigned id) {
  switch (id) {
    case 0: return test_loader_valid();
    case 1: return test_loader_errors();
    case 2: return test_queue_transaction();
    case 3: return test_all_quanta_and_owned_code();
    case 4: return test_cancellation_and_stale();
    case 5: return test_instruction_revocation_and_reentry();
    case 6: return test_trap_and_no_replay();
    case 7: return test_snapshot_restart_and_rollback();
    case 8: return test_commit_boundary();
    case 9: return test_argument_boundaries();
    case 10: return test_exhausted_revision_restore();
    case 11: return test_copied_instance_has_no_authority();
    default: return -1;
  }
}
int pet_program_runtime_selftest(void) {
  for (unsigned i = 0; i < 12; i++) { int r = pet_program_runtime_case(i); if (r) return r; }
  return 0;
}
#ifndef PET_PROGRAM_WASM
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  int r = argc > 1 ? pet_program_runtime_case((unsigned)strtoul(argv[1],NULL,10)) : pet_program_runtime_selftest();
  if (!r && argc == 1) r = pet_program_runtime_fuzz(10000);
  if (r) { fprintf(stderr,"failed at line %d\n",r); return 1; }
  printf("{\"ok\":true,\"imageBytes\":%zu,\"runtimeBytes\":%zu,\"snapshotBytes\":%zu,\"reportBytes\":%zu}\n",
    sizeof(pet_program_image_t),sizeof(pet_program_runtime_t),sizeof(pet_program_snapshot_t),sizeof(pet_program_report_t));
  return 0;
}
#endif
