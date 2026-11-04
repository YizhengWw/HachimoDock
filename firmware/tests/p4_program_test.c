/* The same tests run as native ASan/UBSan C and freestanding WASM C. */
#include "pet_p4_program_core.h"
#include "p4_program_fixture.h"
#include <limits.h>

#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
typedef struct { uint64_t generation; uint32_t calls, cancel_at; bool live; } guard_t;
static bool owner_current(void *context, uint64_t generation) {
  guard_t *guard = context;
  guard->calls++;
  return guard->live && guard->generation == generation
    && (!guard->cancel_at || guard->calls < guard->cancel_at);
}
static pet_program_authority_t authority(guard_t *guard) {
  pet_program_authority_t a = {guard->generation, owner_current, guard,
    &pet_program_collection_grant, 1};
  return a;
}
static bool bytes_equal(const void *a, const void *b, size_t count) {
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < count; i++) if (x[i] != y[i]) return false;
  return true;
}
static void bytes_copy(void *a, const void *b, size_t count) {
  unsigned char *x = a;
  const unsigned char *y = b;
  for (size_t i = 0; i < count; i++) x[i] = y[i];
}
static int test_collection_and_outbox(void) {
  guard_t g = {7, 0, 0, true};
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state, before;
  pet_program_workspace_t w;
  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_validate(&pet_program_collection) == PET_PROGRAM_READY);
  CHECK(pet_program_run(&pet_program_collection, &pet_program_collection_event, &state, &a, &w)
    == PET_PROGRAM_READY);
  CHECK(bytes_equal(&state, &before, sizeof(state))); /* READY is not publication. */
  CHECK(w.candidate.outputs[0] == 51 && w.candidate.outputs[1] == 4);
  CHECK(w.candidate.cells[8] == 9 && w.candidate.cells[9] == 16
    && w.candidate.cells[10] == 25 && w.candidate.cells[11] == 1);
  CHECK(w.candidate.outbox_count == 1 && w.candidate.outbox[0].capability == 42
    && w.candidate.outbox[0].operation == 3 && w.candidate.outbox[0].first == 51
    && w.candidate.outbox[0].second == 4);
  CHECK(w.used_fuel == 89 && g.calls == (uint32_t)w.used_fuel + 1);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_COMMITTED);
  CHECK(state.revision == 1 && state.outputs[0] == 51 && state.outbox_count == 1);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(pet_program_run(&pet_program_collection, &pet_program_collection_event, &state, &a, &w)
    == PET_PROGRAM_OUTBOX_BUSY); /* Includes externally failed/unknown: no implicit retry. */
  CHECK(!pet_program_ack_outbox(&state, 0, &a));
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  g.live = false;
  CHECK(!pet_program_ack_outbox(&state, 1, &a));
  g.live = true;
  CHECK(pet_program_ack_outbox(&state, 1, &a));
  CHECK(state.revision == 2 && state.outbox_count == 0);
  for (size_t i = 0; i < sizeof(state.outbox); i++)
    CHECK(((unsigned char *)state.outbox)[i] == 0);
  CHECK(!pet_program_ack_outbox(&state, 2, &a));
  CHECK(pet_program_run(&pet_program_collection, &pet_program_collection_event, &state, &a, &w)
    == PET_PROGRAM_READY);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_COMMITTED);
  CHECK(state.revision == 3);
  return 0;
}
static int test_initialization_and_validation(void) {
  pet_program_state_t state, expected;
  unsigned char *raw = (unsigned char *)&state;
  for (size_t i = 0; i < sizeof(state); i++) raw[i] = 0xa5;
  bytes_copy(&expected, &state, sizeof(state));
  CHECK(!pet_program_init(&state, 0) && !pet_program_init(NULL, 1));
  CHECK(bytes_equal(&state, &expected, sizeof(state)));
  CHECK(pet_program_init(&state, 1));
  for (size_t i = sizeof(state.generation); i < sizeof(state); i++) CHECK(raw[i] == 0);
  CHECK(state.generation == 1);
  pet_program_insn_t code[2] = {{PET_OP_HALT, 0, 0, 0, 0}, {PET_OP_HALT, 0, 0, 0, 0}};
  pet_program_t p = {code, 2, 2, 0, 0};
  CHECK(pet_program_validate(NULL) == PET_PROGRAM_INVALID);
  CHECK(pet_program_validate(&p) == PET_PROGRAM_READY);
  code[1].op = 255; /* Unreachable bytes are structurally validated too. */
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  code[1] = (pet_program_insn_t){PET_OP_JUMP, 0, 0, 0, 2};
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  code[1].imm = -1;
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  code[1] = (pet_program_insn_t){PET_OP_CONST, PET_PROGRAM_REGS, 0, 0, 0};
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  code[1] = (pet_program_insn_t){PET_OP_HALT, 0, 1, 0, 0};
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  code[1] = code[0];
  p.fuel = 0; CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  p.fuel = PET_PROGRAM_FUEL + 1; CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  p.fuel = 2; p.count = PET_PROGRAM_CODE + 1;
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  p.count = 2; p.cells = PET_PROGRAM_CELLS + 1;
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  p.cells = 0; p.intent_limit = PET_PROGRAM_INTENTS + 1;
  CHECK(pet_program_validate(&p) == PET_PROGRAM_INVALID);
  return 0;
}
static int expect_failure(const pet_program_t *p, pet_program_result_t result) {
  guard_t g = {7, 0, 0, true};
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state, before;
  pet_program_workspace_t w;
  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_run(p, &pet_program_collection_event, &state, &a, &w) == result);
  CHECK(!w.ready && w.result == result && w.candidate.outbox_count == 0);
  CHECK(w.used_fuel <= p->fuel);
  if (result == PET_PROGRAM_FUEL_EXHAUSTED) CHECK(w.used_fuel == p->fuel);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  return 0;
}
static int test_traps(void) {
  pet_program_insn_t code[] = {
    {PET_OP_CONST, 0, 0, 0, 0}, {PET_OP_CONST, 1, 0, 0, 99},
    {PET_OP_STORE, 0, 1, 0, 0}, {PET_OP_OUTPUT, 0, 1, 0, 0},
    {PET_OP_INTENT, 0, 1, 0, 3}, /* Mutations precede each trap. */
    {PET_OP_CONST, 2, 0, 0, INT32_MIN}, {PET_OP_CONST, 3, 0, 0, -1},
    {PET_OP_DIV, 4, 2, 3, 0}, {PET_OP_HALT, 0, 0, 0, 0},
  };
  pet_program_t p = {code, sizeof(code) / sizeof(code[0]), 32, 1, 1};
  CHECK(expect_failure(&p, PET_PROGRAM_OVERFLOW) == 0);
  code[7].op = PET_OP_MOD; CHECK(expect_failure(&p, PET_PROGRAM_OVERFLOW) == 0);
  code[6].imm = 0; CHECK(expect_failure(&p, PET_PROGRAM_DIV_ZERO) == 0);
  code[7].op = PET_OP_DIV; CHECK(expect_failure(&p, PET_PROGRAM_DIV_ZERO) == 0);
  code[7].op = PET_OP_SUB; code[6].imm = 1;
  CHECK(expect_failure(&p, PET_PROGRAM_OVERFLOW) == 0);
  code[7].op = PET_OP_ADD; code[5].imm = INT32_MAX;
  CHECK(expect_failure(&p, PET_PROGRAM_OVERFLOW) == 0);
  code[7].op = PET_OP_MUL; code[6].imm = 2;
  CHECK(expect_failure(&p, PET_PROGRAM_OVERFLOW) == 0);
  code[7] = (pet_program_insn_t){PET_OP_LOAD, 4, 2, 0, 0}; code[5].imm = -1;
  CHECK(expect_failure(&p, PET_PROGRAM_BOUNDS) == 0);
  code[5].imm = p.cells; CHECK(expect_failure(&p, PET_PROGRAM_BOUNDS) == 0);
  code[7] = (pet_program_insn_t){PET_OP_STORE, 2, 3, 0, 0};
  CHECK(expect_failure(&p, PET_PROGRAM_BOUNDS) == 0);
  code[5].imm = -1; CHECK(expect_failure(&p, PET_PROGRAM_BOUNDS) == 0);
  code[7] = (pet_program_insn_t){PET_OP_EVENT, 4, 0, 0, 1};
  CHECK(expect_failure(&p, PET_PROGRAM_BOUNDS) == 0);
  code[7] = (pet_program_insn_t){PET_OP_INTENT, 0, 2, 3, 3};
  CHECK(expect_failure(&p, PET_PROGRAM_CALL_LIMIT) == 0);
  p.intent_limit = 2; code[7].imm = 4;
  CHECK(expect_failure(&p, PET_PROGRAM_CAPABILITY) == 0);
  code[7].imm = 3; code[7].a = 1;
  CHECK(expect_failure(&p, PET_PROGRAM_CAPABILITY) == 0);
  code[7] = (pet_program_insn_t){PET_OP_JUMP, 0, 0, 0, 7};
  CHECK(expect_failure(&p, PET_PROGRAM_FUEL_EXHAUSTED) == 0);
  p.fuel = PET_PROGRAM_FUEL;
  CHECK(expect_failure(&p, PET_PROGRAM_FUEL_EXHAUSTED) == 0);
  code[7] = (pet_program_insn_t){PET_OP_MOVE, 0, 1, 0, 0}; p.count = 8;
  CHECK(expect_failure(&p, PET_PROGRAM_BAD_PC) == 0);
  return 0;
}
static int test_cancel_conflict_and_reentry(void) {
  pet_program_insn_t code[] = {
    {PET_OP_CONST, 0, 0, 0, 77}, {PET_OP_STORE, 1, 0, 0, 0},
    {PET_OP_INTENT, 0, 0, 1, 3}, {PET_OP_HALT, 0, 0, 0, 0},
  };
  pet_program_t p = {code, 4, 4, 1, 1};
  guard_t g = {7, 0, 5, true}; /* Revoke after STORE and INTENT, before HALT. */
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state, before;
  pet_program_workspace_t w;
  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_CANCELLED);
  CHECK(w.used_fuel == 3 && !w.ready && w.candidate.outbox_count == 0);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  g.cancel_at = 0;
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_READY);
  g.live = false;
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_CANCELLED);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  g.live = true;
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_READY);
  a.grant_count = 0; /* Current owner, but its capability was revoked after run. */
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_CAPABILITY);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  a = authority(&g);
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_READY);
  state.revision++; bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_CONFLICT);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_READY);
  /* Reentry has a new trusted owner generation, not an equal component string. */
  g.generation++; a.generation++; state.generation++;
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_CANCELLED);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_READY);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_COMMITTED);
  CHECK(state.cells[0] == 77 && state.revision == 2 && state.outbox_count == 1);
  g.live = false; /* Cancellation after publication never rolls back or relabels it. */
  CHECK(w.result == PET_PROGRAM_COMMITTED && state.cells[0] == 77);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  g.live = true;
  CHECK(pet_program_ack_outbox(&state, 2, &a));
  state.revision = UINT32_MAX;
  CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_OVERFLOW);
  return 0;
}
static int test_authority_event_and_limit_edges(void) {
  pet_program_insn_t code[PET_PROGRAM_CODE];
  for (unsigned i = 0; i < PET_PROGRAM_CODE; i++)
    code[i] = (pet_program_insn_t){PET_OP_CONST, 0, 0, 0, 63};
  code[252] = (pet_program_insn_t){PET_OP_EVENT, 15, 0, 0, 3};
  code[253] = (pet_program_insn_t){PET_OP_STORE, 0, 15, 0, 0};
  code[254] = (pet_program_insn_t){PET_OP_OUTPUT, 7, 15, 0, 0};
  code[255] = (pet_program_insn_t){PET_OP_HALT, 0, 0, 0, 0};
  pet_program_t p = {code, PET_PROGRAM_CODE, PET_PROGRAM_CODE, PET_PROGRAM_CELLS, 0};
  guard_t g = {7, 0, 0, true};
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state, before;
  pet_program_workspace_t w;
  pet_program_event_t e = {4, {1, 2, 3, INT32_MIN}};
  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_READY);
  CHECK(w.used_fuel == 256 && w.candidate.cells[63] == INT32_MIN
    && w.candidate.outputs[7] == INT32_MIN);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  e.count = PET_PROGRAM_EVENT_WORDS + 1;
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_INVALID);
  e.count = 4;
  a.current = NULL;
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_INVALID);
  a = authority(&g); a.grant_count = PET_PROGRAM_GRANTS + 1;
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_INVALID);
  pet_program_grant_t grants[4] = {{1, 1}, {2, 1}, {3, 1}, {4, 1U << 15}};
  a = authority(&g); a.grants = grants; a.grant_count = 4;
  grants[3].id = 1;
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_INVALID);
  grants[3].id = 4;
  /* Exact call limit, last grant and highest operation bit; no ambient IO. */
  code[0] = (pet_program_insn_t){PET_OP_CONST, 15, 0, 0, INT32_MAX};
  for (unsigned i = 1; i <= PET_PROGRAM_INTENTS; i++)
    code[i] = (pet_program_insn_t){PET_OP_INTENT, 3, 15, 15, 15};
  code[9] = (pet_program_insn_t){PET_OP_HALT, 0, 0, 0, 0};
  p.count = 10; p.intent_limit = PET_PROGRAM_INTENTS;
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_READY);
  CHECK(w.candidate.outbox_count == 8 && w.candidate.outbox[7].capability == 4
    && w.candidate.outbox[7].operation == 15 && w.candidate.outbox[7].second == INT32_MAX);
  code[9] = code[8]; code[10] = (pet_program_insn_t){PET_OP_HALT, 0, 0, 0, 0}; p.count = 11;
  CHECK(pet_program_run(&p, &e, &state, &a, &w) == PET_PROGRAM_CALL_LIMIT);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  return 0;
}
static int test_sliced_execution_and_snapshot(void) {
  guard_t g = {7, 0, 0, true};
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state, before;
  pet_program_workspace_t w, legacy;
  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_run(&pet_program_collection, &pet_program_collection_event, &state, &a, &legacy) == PET_PROGRAM_READY);
  pet_program_t descriptor = pet_program_collection;
  pet_program_event_t event = pet_program_collection_event;
  CHECK(pet_program_begin(&descriptor, &event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(w.used_fuel == 0 && !w.ready);
  descriptor.count = 0; event.count = 0; /* The descriptor/event were copied. */
  CHECK(pet_program_step(&w, &a, PET_PROGRAM_QUANTUM) == PET_PROGRAM_RUNNING);
  CHECK(w.used_fuel == 64 && !w.ready);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(w.result == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, PET_PROGRAM_QUANTUM) == PET_PROGRAM_READY);
  CHECK(w.used_fuel == 89 && bytes_equal(&w.candidate, &legacy.candidate, sizeof(state)));
  CHECK(pet_program_step(&w, &a, 1) == PET_PROGRAM_NOT_READY && w.ready);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_COMMITTED);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_NOT_READY);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(w.result == PET_PROGRAM_COMMITTED && bytes_equal(&state, &before, sizeof(state)));

  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  CHECK(pet_program_begin(&pet_program_collection, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_RUNNING);
  g.live = false;
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_CANCELLED && w.used_fuel == 64);
  g.live = true; /* Idle/reentry cannot resurrect a terminal workspace. */
  CHECK(pet_program_step(&w, &a, 1) == PET_PROGRAM_NOT_READY);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  CHECK(pet_program_begin(&pet_program_collection, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_RUNNING);
  state.revision++; /* Only commit consults the independently owned live model. */
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_READY);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_CONFLICT);
  CHECK(state.revision == 1 && !state.outbox_count && !state.outputs[0]);
  CHECK(pet_program_begin(&pet_program_collection, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_RUNNING);
  a.generation++; g.generation++;
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_CANCELLED);
  CHECK(!state.outbox_count && !state.outputs[0]);
  return 0;
}
static int test_sliced_fuel_and_traps(void) {
  guard_t g = {7, 0, 0, true};
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state, before;
  pet_program_workspace_t w;
  pet_program_collection_init(&state, g.generation);
  bytes_copy(&before, &state, sizeof(state));
  pet_program_insn_t code[66];
  for (unsigned i = 0; i < 66; i++) code[i] = (pet_program_insn_t){PET_OP_MOVE, 3, 0, 0, 0};
  code[0] = (pet_program_insn_t){PET_OP_CONST, 0, 0, 0, INT32_MIN};
  code[1] = (pet_program_insn_t){PET_OP_CONST, 1, 0, 0, -1};
  code[2] = (pet_program_insn_t){PET_OP_STORE, 2, 0, 0, 0};
  code[64] = (pet_program_insn_t){PET_OP_DIV, 4, 0, 1, 0};
  code[65] = (pet_program_insn_t){PET_OP_HALT, 0, 0, 0, 0};
  pet_program_t p = {code, 66, 66, 1, 0};
  CHECK(pet_program_begin(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_RUNNING && w.candidate.cells[0] == INT32_MIN);
  CHECK(pet_program_step(&w, &a, 64) == PET_PROGRAM_OVERFLOW && w.used_fuel == 65);
  CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  code[0] = (pet_program_insn_t){PET_OP_JUMP, 0, 0, 0, 0}; p.count = 1; p.fuel = PET_PROGRAM_FUEL;
  for (unsigned quantum = 1; quantum <= PET_PROGRAM_QUANTUM; quantum++) {
    CHECK(pet_program_begin(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
    unsigned slices = 0;
    pet_program_result_t result;
    do {
      unsigned previous = w.used_fuel;
      result = pet_program_step(&w, &a, (uint16_t)quantum);
      CHECK(w.used_fuel > previous && w.used_fuel - previous <= quantum);
      CHECK(++slices <= PET_PROGRAM_FUEL);
    } while (result == PET_PROGRAM_RUNNING);
    CHECK(result == PET_PROGRAM_FUEL_EXHAUSTED && w.used_fuel == PET_PROGRAM_FUEL);
    CHECK(slices == (PET_PROGRAM_FUEL + quantum - 1) / quantum);
    CHECK(pet_program_step(&w, &a, 1) == PET_PROGRAM_NOT_READY);
  }
  CHECK(pet_program_begin(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, 0) == PET_PROGRAM_INVALID && w.used_fuel == 0);
  CHECK(pet_program_begin(&p, &pet_program_collection_event, &state, &a, &w) == PET_PROGRAM_RUNNING);
  CHECK(pet_program_step(&w, &a, PET_PROGRAM_QUANTUM + 1) == PET_PROGRAM_INVALID);
  CHECK(pet_program_step(NULL, &a, 1) == PET_PROGRAM_INVALID);
  CHECK(bytes_equal(&state, &before, sizeof(state)));
  return 0;
}
int pet_program_selftest(void) {
  int result = test_initialization_and_validation(); if (result) return result;
  result = test_collection_and_outbox(); if (result) return result;
  result = test_traps(); if (result) return result;
  result = test_authority_event_and_limit_edges(); if (result) return result;
  result = test_cancel_conflict_and_reentry(); if (result) return result;
  result = test_sliced_execution_and_snapshot(); if (result) return result;
  return test_sliced_fuel_and_traps();
}
static uint32_t random_word(uint32_t *seed) {
  *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
  return *seed;
}
static int32_t random_i32(uint32_t *seed) {
  uint32_t u = random_word(seed);
  return u <= INT32_MAX ? (int32_t)u : (int32_t)((int64_t)u - INT64_C(4294967296));
}
int pet_program_fuzz(uint32_t trials) {
  uint32_t seed = UINT32_C(0xc17c0de);
  for (uint32_t n = 0; n < trials; n++) {
    guard_t g = {7, 0, 0, true};
    pet_program_authority_t a = authority(&g);
    pet_program_state_t state, before;
    pet_program_workspace_t w;
    pet_program_collection_init(&state, g.generation);
    bytes_copy(&before, &state, sizeof(state));
    pet_program_insn_t code[32];
    pet_program_t p = {code, 32, (uint16_t)(1 + random_word(&seed) % 256), 16, 4};
    for (unsigned i = 0; i < p.count; i++) {
      code[i] = (pet_program_insn_t){(uint8_t)(random_word(&seed) % PET_OP_COUNT),
        (uint8_t)(random_word(&seed) % 16), (uint8_t)(random_word(&seed) % 16),
        (uint8_t)(random_word(&seed) % 16), 0};
      switch (code[i].op) {
        case PET_OP_HALT: code[i].a = code[i].b = code[i].c = 0; break;
        case PET_OP_CONST: code[i].b = code[i].c = 0; code[i].imm = random_i32(&seed); break;
        case PET_OP_MOVE: case PET_OP_LOAD: case PET_OP_STORE: code[i].c = 0; break;
        case PET_OP_JUMP: code[i].a = 0; /* fall through */
        case PET_OP_JZ: code[i].b = code[i].c = 0; code[i].imm = (int32_t)(random_word(&seed) % 32); break;
        case PET_OP_EVENT: code[i].b = code[i].c = 0; code[i].imm = (int32_t)(random_word(&seed) % 4); break;
        case PET_OP_OUTPUT: code[i].a %= 8; code[i].c = 0; break;
        case PET_OP_INTENT: code[i].a %= 4; code[i].imm = (int32_t)(random_word(&seed) % 16); break;
        default: break;
      }
    }
    if (n % 3 == 0) code[random_word(&seed) % 32].op = 255;
    if (n % 5 == 0) g.cancel_at = 2 + random_word(&seed) % 16;
    pet_program_result_t result = pet_program_run(&p, &pet_program_collection_event, &state, &a, &w);
    CHECK(bytes_equal(&state, &before, sizeof(state)));
    CHECK(w.used_fuel <= p.fuel);
    /* Same code/event and fresh cancellation observations, but arbitrary slice
     * size: terminal result, total fuel and candidate must match legacy run. */
    pet_program_workspace_t sliced;
    g.calls = 0;
    pet_program_result_t sliced_result = pet_program_begin(&p, &pet_program_collection_event, &state, &a, &sliced);
    unsigned sliced_calls = 0;
    while (sliced_result == PET_PROGRAM_RUNNING) {
      sliced_result = pet_program_step(&sliced, &a, (uint16_t)(1 + n % PET_PROGRAM_QUANTUM));
      CHECK(++sliced_calls <= p.fuel);
    }
    CHECK(sliced_result == result && sliced.used_fuel == w.used_fuel);
    CHECK(bytes_equal(&sliced.candidate, &w.candidate, sizeof(state)));
    if (result == PET_PROGRAM_READY) {
      CHECK(w.ready && w.candidate.outbox_count <= p.intent_limit);
      pet_program_result_t committed = pet_program_commit(&state, &w, &a);
      CHECK(committed == PET_PROGRAM_COMMITTED || committed == PET_PROGRAM_CANCELLED);
      if (committed == PET_PROGRAM_CANCELLED) CHECK(bytes_equal(&state, &before, sizeof(state)));
    } else {
      CHECK(!w.ready && w.candidate.outbox_count == 0);
      CHECK(pet_program_commit(&state, &w, &a) == PET_PROGRAM_NOT_READY);
    }
    /* Independent arithmetic oracle, including valid executions on all operators. */
    g.cancel_at = 0;
    int32_t left = random_i32(&seed), right = random_i32(&seed);
    uint8_t op = (uint8_t)(PET_OP_ADD + n % 7);
    code[0] = (pet_program_insn_t){PET_OP_CONST, 0, 0, 0, left};
    code[1] = (pet_program_insn_t){PET_OP_CONST, 1, 0, 0, right};
    code[2] = (pet_program_insn_t){op, 2, 0, 1, 0};
    code[3] = (pet_program_insn_t){PET_OP_OUTPUT, 0, 2, 0, 0};
    code[4] = (pet_program_insn_t){PET_OP_HALT, 0, 0, 0, 0};
    p.count = 5; p.fuel = 5;
    pet_program_collection_init(&state, g.generation);
    int64_t expected = 0;
    pet_program_result_t want = PET_PROGRAM_READY;
    switch (op) {
      case PET_OP_ADD: expected = (int64_t)left + right; break;
      case PET_OP_SUB: expected = (int64_t)left - right; break;
      case PET_OP_MUL: expected = (int64_t)left * right; break;
      case PET_OP_DIV: case PET_OP_MOD:
        if (!right) want = PET_PROGRAM_DIV_ZERO;
        else if (left == INT32_MIN && right == -1) want = PET_PROGRAM_OVERFLOW;
        else expected = op == PET_OP_DIV ? (int64_t)left / right : (int64_t)left % right;
        break;
      case PET_OP_LT: expected = left < right; break;
      case PET_OP_EQ: expected = left == right; break;
      default: return __LINE__;
    }
    if (expected < INT32_MIN || expected > INT32_MAX) want = PET_PROGRAM_OVERFLOW;
    CHECK(pet_program_run(&p, &pet_program_collection_event, &state, &a, &w) == want);
    if (want == PET_PROGRAM_READY) CHECK(w.candidate.outputs[0] == expected);
  }
  return 0;
}
/* Trusted test-only entry. No IO/driver calls: intents stay inert. P4 adapters
 * should time individual run+commit calls with their actual broker check. */
uint32_t pet_program_bench(uint32_t iterations) {
  uint32_t checksum = 0;
  guard_t g = {7, 0, 0, true};
  pet_program_authority_t a = authority(&g);
  pet_program_state_t state;
  pet_program_workspace_t w;
  for (uint32_t i = 0; i < iterations; i++) {
    pet_program_collection_init(&state, g.generation);
    if (pet_program_run(&pet_program_collection, &pet_program_collection_event, &state, &a, &w)
        != PET_PROGRAM_READY || pet_program_commit(&state, &w, &a) != PET_PROGRAM_COMMITTED) return 0;
    checksum += (uint32_t)state.outputs[0] + w.used_fuel;
  }
  return checksum;
}
#ifndef PET_PROGRAM_WASM
#include <stdio.h>
#include <time.h>
int main(void) {
  int test = pet_program_selftest();
  if (test) { fprintf(stderr, "selftest failed at line %d\n", test); return 1; }
  test = pet_program_fuzz(50000);
  if (test) { fprintf(stderr, "fuzz failed at line %d\n", test); return 1; }
  clock_t start = clock();
  uint32_t checksum = pet_program_bench(10000);
  double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
  if (checksum != 1400000) { fprintf(stderr, "bad benchmark checksum %u\n", checksum); return 1; }
  printf("{\"selftest\":true,\"fuzzTrials\":50000,\"benchmarkIterations\":10000,"
    "\"checksum\":%u,\"cpuSeconds\":%.6f,\"instructionBytes\":%zu,"
    "\"stateBytes\":%zu,\"workspaceBytes\":%zu,\"eventBytes\":%zu}\n",
    checksum, seconds, sizeof(pet_program_insn_t), sizeof(pet_program_state_t),
    sizeof(pet_program_workspace_t), sizeof(pet_program_event_t));
  return 0;
}
#endif
