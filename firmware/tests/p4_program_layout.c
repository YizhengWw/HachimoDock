/* Object-only target ABI measurements. Never linked into production firmware. */
#include "pet_p4_program_core.h"
const unsigned char pet_program_layout_state[sizeof(pet_program_state_t)] = {0};
const unsigned char pet_program_layout_workspace[sizeof(pet_program_workspace_t)] = {0};
const unsigned char pet_program_layout_instruction[sizeof(pet_program_insn_t)] = {0};
const unsigned char pet_program_layout_descriptor[sizeof(pet_program_t)] = {0};
const unsigned char pet_program_layout_event[sizeof(pet_program_event_t)] = {0};
const unsigned char pet_program_layout_authority[sizeof(pet_program_authority_t)] = {0};
const unsigned char pet_program_layout_grant[sizeof(pet_program_grant_t)] = {0};
