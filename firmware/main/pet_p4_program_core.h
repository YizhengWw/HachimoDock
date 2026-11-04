/* Isolated experimental integer VM. NOT an installable package capability.
 * No heap, OS, drivers, network, audio, callbacks to untrusted code or recursion.
 * The trusted adapter owns all pointers and serializes state/grant/code access.
 * Code storage stays immutable and alive through all execution slices. Begin
 * copies the descriptor/event/source; grants stay immutable during each call.
 * workspace must not alias any input and is private to one invocation. An
 * adapter may release its state lock during execution only against an immutable
 * source snapshot, with current() checking the live generation. Commit must use
 * the live state and recheck its revision under the owner+state lock.
 * It must hold the same owner+state lock throughout commit and outbox ack;
 * current() is a bounded broker generation/lease check. It may latch observed
 * cancellation by revoking its own authority; it must not change model state,
 * publish external effects, wait on IO/scheduler resources or invoke untrusted code.
 * Bytecode cannot construct grants, owner generations, pointers or revisions.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_p4_component_features.h"

#define PET_PROGRAM_REGS 16U
#if PET_P4_PROGRAM_V2
/* p4-component-program-v2: wider pure-compute budgets, same quantum/slice policy.
 * v1 images (<=64 cells, <=256 instructions, <=4096 fuel) stay valid. */
#define PET_PROGRAM_CELLS 256U
#define PET_PROGRAM_CODE 1024U
#if PET_P4_PUBLIC_PROGRAM_PROFILE
#define PET_PROGRAM_FUEL 4096U
#else
#define PET_PROGRAM_FUEL 16384U
#endif
typedef uint16_t pet_program_cells_t;
#else
#define PET_PROGRAM_CELLS 64U
#define PET_PROGRAM_CODE 256U
#define PET_PROGRAM_FUEL 4096U
typedef uint8_t pet_program_cells_t;
#endif
/* v1 budgets, also the ceiling of a v1 image under a v2 build. */
#define PET_PROGRAM_V1_CELLS 64U
#define PET_PROGRAM_V1_CODE 256U
#define PET_PROGRAM_V1_FUEL 4096U
#define PET_PROGRAM_QUANTUM 64U
#define PET_PROGRAM_EVENT_WORDS 4U
#define PET_PROGRAM_OUTPUTS 8U
#define PET_PROGRAM_GRANTS 4U
#define PET_PROGRAM_INTENTS 8U

typedef enum {
  PET_OP_HALT, PET_OP_CONST, PET_OP_MOVE, PET_OP_ADD, PET_OP_SUB, PET_OP_MUL,
  PET_OP_DIV, PET_OP_MOD, PET_OP_LT, PET_OP_EQ, PET_OP_JUMP, PET_OP_JZ,
  PET_OP_LOAD, PET_OP_STORE, PET_OP_EVENT, PET_OP_OUTPUT, PET_OP_INTENT,
  /* v2 only (rejected by a v1 build / v1 image): TEXT a,b selects a bounded text
   * table index; CELLGET a,b,c / CELLSET a,b,c address the mirrored p4-cells-v1
   * grid as cells[y*cols+x] with x,y in registers (trap BOUNDS outside). */
  PET_OP_TEXT, PET_OP_CELLGET, PET_OP_CELLSET,
  PET_OP_COUNT
} pet_program_opcode_t;
/* Explicit 8-byte instruction; an eventual wire loader must decode LE fields,
 * not cast byte buffers into this native struct. No such loader is installed. */
typedef struct { uint8_t op, a, b, c; int32_t imm; } pet_program_insn_t;
typedef struct {
  const pet_program_insn_t *code;
  uint16_t count, fuel;
  pet_program_cells_t cells;
  uint8_t intent_limit;
#if PET_P4_PROGRAM_V2
  /* Host-supplied (package cells/texts), never wire data: 0 = no grid / no table. */
  uint8_t grid_cols, grid_rows, texts;
#endif
} pet_program_t;
typedef struct { uint8_t count; int32_t words[PET_PROGRAM_EVENT_WORDS]; } pet_program_event_t;
/* Restrictive native output projection, never a grant or a callback. A host
 * derives this from its actual receiving model, not a program's claim that a
 * value is valid. Immutable/nonoverlapping for the whole owner-locked call. */
typedef struct {int32_t minimum[PET_PROGRAM_OUTPUTS],maximum[PET_PROGRAM_OUTPUTS];} pet_program_output_contract_t;
bool pet_program_output_contract_valid(const pet_program_output_contract_t *contract);
bool pet_program_outputs_fit(const int32_t outputs[PET_PROGRAM_OUTPUTS],const pet_program_output_contract_t *contract);
typedef struct { uint32_t id; uint16_t operations; } pet_program_grant_t;
typedef struct { uint32_t capability; uint16_t operation; int32_t first, second; } pet_program_intent_t;
typedef struct {
  uint64_t generation;
  bool (*current)(void *context, uint64_t generation);
  void *context;
  const pet_program_grant_t *grants;
  uint8_t grant_count;
} pet_program_authority_t;
#if PET_P4_PROGRAM_V2
typedef struct pet_program_publication pet_program_publication_t;
#endif
typedef struct pet_program_state {
  uint64_t generation;
  uint32_t revision;
  int32_t cells[PET_PROGRAM_CELLS], outputs[PET_PROGRAM_OUTPUTS];
  pet_program_intent_t outbox[PET_PROGRAM_INTENTS];
  uint8_t outbox_count;
#if PET_P4_PROGRAM_V2
  /* Native facade only. Plain values/candidates have NULL here. The selected
   * immutable slot is authoritative; facade arrays must not be read directly
   * after binding. External owner/model serialization covers all readers. */
  pet_program_publication_t *publication;
#endif
} pet_program_state_t;
#if PET_P4_PROGRAM_V2
struct pet_program_publication {
  const void *self;
  pet_program_state_t *target, *slots[2], *live;
  bool preparing;
};
/* Fresh/inert storage only, outside the broker mux. No heap/global scratch.
 * All three states and the selector must be disjoint, fixed-address, and live
 * until physical retirement. Never reset/rebind while a reader/writer exists. */
bool pet_program_state_bind_publication(pet_program_state_t *target,
  pet_program_publication_t *selector,pet_program_state_t slots[2]);
#endif
/* Caller holds its model lock, including the complete read/copy. Revocation
 * may occur independently but never mutates the selected immutable payload. */
const pet_program_state_t *pet_program_state_read(const pet_program_state_t *state);
/* Trusted native publication boundary, never supplied by bytecode/wire data.
 * It must atomically recheck the SAME exact owner and either publish this whole
 * fixed state or change nothing. V1 copies <=512 bytes under the broker mux;
 * V2 stages a spare outside it, then flips its caller-owned selector. Bound
 * facades must be read through state_read, never their stale inline arrays.
 * No IO/wait/untrusted callback or partial copy inside the broker mux.
 * Parent still serializes model/revision and keeps both buffers disjoint. */
typedef bool (*pet_program_publish_fn)(void *context,uint64_t generation,
  pet_program_state_t *target,const pet_program_state_t *candidate);
typedef enum {
  PET_PROGRAM_EMPTY, PET_PROGRAM_READY, PET_PROGRAM_COMMITTED,
  PET_PROGRAM_INVALID, PET_PROGRAM_BAD_PC, PET_PROGRAM_FUEL_EXHAUSTED,
  PET_PROGRAM_DIV_ZERO, PET_PROGRAM_OVERFLOW, PET_PROGRAM_BOUNDS,
  PET_PROGRAM_CANCELLED, PET_PROGRAM_CAPABILITY, PET_PROGRAM_CALL_LIMIT,
  PET_PROGRAM_CONFLICT, PET_PROGRAM_OUTBOX_BUSY, PET_PROGRAM_NOT_READY,
  PET_PROGRAM_RUNNING
} pet_program_result_t;
typedef struct {
  pet_program_state_t candidate;
  int32_t registers[PET_PROGRAM_REGS];
  uint32_t base_revision;
  uint16_t pc, used_fuel;
  pet_program_result_t result;
  bool ready;
  pet_program_t program;
  pet_program_event_t event;
} pet_program_workspace_t;

/* Explicitly initializes ALL bytes; nonzero generation is trusted, never wire data. */
bool pet_program_init(pet_program_state_t *state, uint64_t generation);
pet_program_result_t pet_program_validate(const pet_program_t *program);
/* Initializes a private invocation without executing instructions. A fresh
 * begin explicitly discards this workspace's prior private candidate. The
 * source model is copied; later slices never retain/read a live source pointer. */
pet_program_result_t pet_program_begin(const pet_program_t *program,
  const pet_program_event_t *event, const pet_program_state_t *state,
  const pet_program_authority_t *authority, pet_program_workspace_t *workspace);
/* Executes at most 1..PET_PROGRAM_QUANTUM instructions, retaining pc/registers,
 * candidate and total fuel on RUNNING. Caller owns scheduling/yielding. Fresh
 * authority is checked on EVERY instruction. Terminal/duplicate step is inert. */
pet_program_result_t pet_program_step(pet_program_workspace_t *workspace,
  const pet_program_authority_t *authority, uint16_t max_fuel);
typedef enum {PET_PROGRAM_CONTINUE,PET_PROGRAM_YIELD,PET_PROGRAM_CANCEL_SLICE} pet_program_checkpoint_result_t;
typedef pet_program_checkpoint_result_t (*pet_program_checkpoint_fn)(void *context);
/* Optional trusted scheduler checkpoint, never supplied by bytecode. YIELD
 * retains private pc/registers/fuel without consuming another instruction;
 * CANCEL is terminal. Each permitted instruction still checks fresh authority
 * AFTER the checkpoint. No callback after HALT/trap and no host effects here.
 * This is cooperative: it cannot preempt a callback, ISR or Flash/cache stall.
 * Callback/context are bounded native code/private storage, immutable for the
 * call, non-reentrant and cannot mutate workspace/authority/source objects. */
pet_program_result_t pet_program_step_with_checkpoint(pet_program_workspace_t *workspace,
  const pet_program_authority_t *authority,uint16_t max_fuel,
  pet_program_checkpoint_fn checkpoint,void *context);
/* One event per invocation. All writes/intents are private until commit succeeds. */
/* Compatibility convenience: drains begin/step synchronously, without yielding.
 * Cooperative device scheduling must use begin/step, not this wrapper. */
pet_program_result_t pet_program_run(const pet_program_t *program,
  const pet_program_event_t *event, const pet_program_state_t *state,
  const pet_program_authority_t *authority, pet_program_workspace_t *workspace);
/* Rechecks current grants as well as generation/revision before publication.
 * No callbacks after publication: cancellation cannot relabel a completed commit.
 * The caller must serialize owner revocation and state mutation across this call.
 * READY/COMMITTED mean local transaction status, never external IO completion. */
pet_program_result_t pet_program_commit(pet_program_state_t *state,
  pet_program_workspace_t *workspace, const pet_program_authority_t *authority);
/* NULL contract preserves the legacy full-int32 behavior. Invalid/out-of-range
 * projections never clamp, partially publish cells/outputs, or advance revision.
 * Alias arguments are rejected before any write. No callbacks after commit. */
pet_program_result_t pet_program_commit_with_contract(pet_program_state_t *state,
  pet_program_workspace_t *workspace,const pet_program_authority_t *authority,
  const pet_program_output_contract_t *contract);
/* Optional native linearization point against independent owner revocation.
 * NULL preserves legacy caller-locked commit. Failure is latched CANCELLED;
 * no callback follows successful publication and no event is retried. */
pet_program_result_t pet_program_commit_with_publication(pet_program_state_t *state,
  pet_program_workspace_t *workspace,const pet_program_authority_t *authority,
  const pet_program_output_contract_t *contract,pet_program_publish_fn publish,void *context);
/* Trusted resolution only. Pending/unknown effects must NOT be automatically
 * resubmitted. Ack only after the adapter has explicitly resolved this outbox. */
bool pet_program_ack_outbox(pet_program_state_t *state, uint32_t revision,
  const pet_program_authority_t *authority);
