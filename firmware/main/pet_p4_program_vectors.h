/* Trusted, compile-time diagnostic vectors shared by probe and host/WASM tests.
 * Not a package loader or installable engine; never accept replacements from IO.
 * One actual filter/map/reduce program: square positive input cells, collect the
 * results in cells 8..15, sum them, and stage one opaque capability intent. */
#pragma once
#include "pet_p4_program_core.h"

static const pet_program_insn_t pet_program_collection_code[] = {
  {PET_OP_CONST, 0, 0, 0, 0},
  {PET_OP_EVENT, 1, 0, 0, 0},
  {PET_OP_CONST, 2, 0, 0, 0},
  {PET_OP_CONST, 3, 0, 0, 0},
  {PET_OP_CONST, 4, 0, 0, 1},
  {PET_OP_CONST, 5, 0, 0, 0},
  {PET_OP_CONST, 6, 0, 0, 8},
  {PET_OP_LT, 7, 0, 1, 0},
  {PET_OP_JZ, 7, 0, 0, 19},
  {PET_OP_LOAD, 8, 0, 0, 0},
  {PET_OP_LT, 9, 5, 8, 0},
  {PET_OP_JZ, 9, 0, 0, 17},
  {PET_OP_MUL, 10, 8, 8, 0},
  {PET_OP_ADD, 11, 6, 2, 0},
  {PET_OP_STORE, 11, 10, 0, 0},
  {PET_OP_ADD, 2, 2, 4, 0},
  {PET_OP_ADD, 3, 3, 10, 0},
  {PET_OP_ADD, 0, 0, 4, 0},
  {PET_OP_JUMP, 0, 0, 0, 7},
  {PET_OP_OUTPUT, 0, 3, 0, 0},
  {PET_OP_OUTPUT, 1, 2, 0, 0},
  {PET_OP_INTENT, 0, 3, 2, 3},
  {PET_OP_HALT, 0, 0, 0, 0},
};
static const pet_program_t pet_program_collection = {
  pet_program_collection_code,
  sizeof(pet_program_collection_code) / sizeof(pet_program_collection_code[0]),
  512, 16, 1,
};
static const pet_program_event_t pet_program_collection_event = {1, {8, 0, 0, 0}};
static const pet_program_grant_t pet_program_collection_grant = {42, 1U << 3};
static inline void pet_program_collection_init(pet_program_state_t *state, uint64_t generation) {
  static const int32_t input[8] = {-2, 3, 0, 4, -1, 5, 1, -3};
  (void)pet_program_init(state, generation);
  for (unsigned i = 0; i < 8; i++) state->cells[i] = input[i];
}
