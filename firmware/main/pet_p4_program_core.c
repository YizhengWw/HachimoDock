#include "pet_p4_program_core.h"
#include <limits.h>

_Static_assert(sizeof(pet_program_insn_t) == 8, "fixed instruction layout");
static void zero_bytes(void *target, size_t count) {
  unsigned char *p = target;
  for (size_t i = 0; i < count; i++) p[i] = 0;
}
static void copy_bytes(void *target, const void *source, size_t count) {
  unsigned char *out = target;
  const unsigned char *in = source;
  for (size_t i = 0; i < count; i++) out[i] = in[i];
}
const pet_program_state_t *pet_program_state_read(const pet_program_state_t *s) {
#if PET_P4_PROGRAM_V2
  if(s&&s->publication){
    const pet_program_publication_t *p=s->publication;
    if(p->self!=p||p->target!=s||(p->live!=p->slots[0]&&p->live!=p->slots[1])||!p->live||p->live->publication)return NULL;
    return p->live;
  }
#endif
  return s;
}
#if PET_P4_PROGRAM_V2
static bool state_overlap(const void *a,size_t an,const void *b,size_t bn){uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;}
bool pet_program_state_bind_publication(pet_program_state_t *target,pet_program_publication_t *p,pet_program_state_t slots[2]){
  if(!target||!p||!slots||!target->generation||target->publication||p->self
      ||state_overlap(target,sizeof(*target),p,sizeof(*p))||state_overlap(target,sizeof(*target),slots,2*sizeof(*slots))
      ||state_overlap(p,sizeof(*p),slots,2*sizeof(*slots)))return false;
  copy_bytes(&slots[0],target,sizeof(*target));zero_bytes(&slots[1],sizeof(*slots));
  *p=(pet_program_publication_t){.self=p,.target=target,.slots={&slots[0],&slots[1]},.live=&slots[0]};
  target->publication=p;return true;
}
#endif
bool pet_program_init(pet_program_state_t *state, uint64_t generation) {
  if (!state || !generation) return false;
  zero_bytes(state, sizeof(*state));
  state->generation = generation;
  return true;
}
static bool authority_valid(const pet_program_authority_t *a) {
  if (!a || !a->generation || !a->current || a->grant_count > PET_PROGRAM_GRANTS
      || (a->grant_count && !a->grants)) return false;
  for (unsigned i = 0; i < a->grant_count; i++) {
    if (!a->grants[i].id || !a->grants[i].operations) return false;
    for (unsigned j = 0; j < i; j++) if (a->grants[i].id == a->grants[j].id) return false;
  }
  return true;
}
static bool current(const pet_program_state_t *state, const pet_program_authority_t *a) {
  state=pet_program_state_read(state);
  return state && authority_valid(a) && state->generation == a->generation
    && a->current(a->context, a->generation);
}
pet_program_result_t pet_program_validate(const pet_program_t *p) {
  if (!p || !p->code || !p->count || p->count > PET_PROGRAM_CODE
      || !p->fuel || p->fuel > PET_PROGRAM_FUEL || p->cells > PET_PROGRAM_CELLS
      || p->intent_limit > PET_PROGRAM_INTENTS) return PET_PROGRAM_INVALID;
  for (unsigned pc = 0; pc < p->count; pc++) {
    const pet_program_insn_t *i = &p->code[pc];
    switch (i->op) {
      case PET_OP_HALT:
        if (i->a || i->b || i->c || i->imm) return PET_PROGRAM_INVALID;
        break;
      case PET_OP_CONST:
        if (i->a >= PET_PROGRAM_REGS || i->b || i->c) return PET_PROGRAM_INVALID;
        break;
      case PET_OP_MOVE: case PET_OP_LOAD: case PET_OP_STORE:
        if (i->a >= PET_PROGRAM_REGS || i->b >= PET_PROGRAM_REGS || i->c || i->imm)
          return PET_PROGRAM_INVALID;
        break;
      case PET_OP_ADD: case PET_OP_SUB: case PET_OP_MUL: case PET_OP_DIV:
      case PET_OP_MOD: case PET_OP_LT: case PET_OP_EQ:
        if (i->a >= PET_PROGRAM_REGS || i->b >= PET_PROGRAM_REGS
            || i->c >= PET_PROGRAM_REGS || i->imm) return PET_PROGRAM_INVALID;
        break;
      case PET_OP_JUMP: case PET_OP_JZ:
        if (i->a >= PET_PROGRAM_REGS || (i->op == PET_OP_JUMP && i->a)
            || i->b || i->c || i->imm < 0 || i->imm >= p->count) return PET_PROGRAM_INVALID;
        break;
      case PET_OP_EVENT:
        if (i->a >= PET_PROGRAM_REGS || i->b || i->c || i->imm < 0
            || (uint32_t)i->imm >= PET_PROGRAM_EVENT_WORDS) return PET_PROGRAM_INVALID;
        break;
      case PET_OP_OUTPUT:
        if (i->a >= PET_PROGRAM_OUTPUTS || i->b >= PET_PROGRAM_REGS || i->c || i->imm)
          return PET_PROGRAM_INVALID;
        break;
      case PET_OP_INTENT:
        if (i->a >= PET_PROGRAM_GRANTS || i->b >= PET_PROGRAM_REGS
            || i->c >= PET_PROGRAM_REGS || i->imm < 0 || i->imm >= 16) return PET_PROGRAM_INVALID;
        break;
#if PET_P4_PROGRAM_V2
      case PET_OP_TEXT:
        if (i->a >= PET_PROGRAM_REGS || i->b >= PET_PROGRAM_REGS || i->c || i->imm) return PET_PROGRAM_INVALID;
        break;
      case PET_OP_CELLGET: case PET_OP_CELLSET:
        if (i->a >= PET_PROGRAM_REGS || i->b >= PET_PROGRAM_REGS || i->c >= PET_PROGRAM_REGS || i->imm
            || !p->grid_cols || !p->grid_rows || (unsigned)p->grid_cols * p->grid_rows > p->cells) return PET_PROGRAM_INVALID;
        break;
#endif
      default: return PET_PROGRAM_INVALID;
    }
  }
  return PET_PROGRAM_READY;
}
static pet_program_result_t fail(pet_program_workspace_t *w, pet_program_result_t result) {
  w->ready = false;
  w->candidate.outbox_count = 0;
  w->result = result;
  return result;
}
pet_program_result_t pet_program_begin(const pet_program_t *p, const pet_program_event_t *e,
    const pet_program_state_t *s, const pet_program_authority_t *a, pet_program_workspace_t *w) {
  s=pet_program_state_read(s);
  if (!w) return PET_PROGRAM_INVALID;
  zero_bytes(w, sizeof(*w));
  if (!s || !e || e->count > PET_PROGRAM_EVENT_WORDS || !authority_valid(a)
      || pet_program_validate(p) != PET_PROGRAM_READY || s->outbox_count > PET_PROGRAM_INTENTS)
    return fail(w, PET_PROGRAM_INVALID);
  if (!current(s, a)) return fail(w, PET_PROGRAM_CANCELLED);
  if (s->outbox_count) return fail(w, PET_PROGRAM_OUTBOX_BUSY);
  if (s->revision == UINT32_MAX) return fail(w, PET_PROGRAM_OVERFLOW);
  copy_bytes(&w->candidate, s, sizeof(*s));
  copy_bytes(&w->program, p, sizeof(*p));
  copy_bytes(&w->event, e, sizeof(*e));
  w->base_revision = s->revision;
  w->result = PET_PROGRAM_RUNNING;
  return w->result;
}
pet_program_result_t pet_program_step_with_checkpoint(pet_program_workspace_t *w,
    const pet_program_authority_t *a,uint16_t max_fuel,pet_program_checkpoint_fn checkpoint,void *context) {
  if (!w) return PET_PROGRAM_INVALID;
  if (w->result != PET_PROGRAM_RUNNING || w->ready) return PET_PROGRAM_NOT_READY;
  if (!max_fuel || max_fuel > PET_PROGRAM_QUANTUM) return fail(w, PET_PROGRAM_INVALID);
  const pet_program_t *p = &w->program;
  const pet_program_event_t *e = &w->event;
  for (unsigned consumed = 0; consumed < max_fuel; consumed++) {
    if (w->pc >= p->count) return fail(w, PET_PROGRAM_BAD_PC);
    if (w->used_fuel >= p->fuel) return fail(w, PET_PROGRAM_FUEL_EXHAUSTED);
    if(checkpoint){
      pet_program_checkpoint_result_t decision=checkpoint(context);
      if(decision==PET_PROGRAM_YIELD)return PET_PROGRAM_RUNNING;
      if(decision==PET_PROGRAM_CANCEL_SLICE)return fail(w,PET_PROGRAM_CANCELLED);
      if(decision!=PET_PROGRAM_CONTINUE)return fail(w,PET_PROGRAM_INVALID);
    }
    // Includes back edges and HALT; no unmetered collection/host operation.
    if (!current(&w->candidate, a)) return fail(w, PET_PROGRAM_CANCELLED);
    const pet_program_insn_t *i = &p->code[w->pc];
    int32_t *r = w->registers;
    int64_t wide = 0;
    w->used_fuel++;
    w->pc++;
    switch (i->op) {
      case PET_OP_HALT:
        w->ready = true; w->result = PET_PROGRAM_READY; return w->result;
      case PET_OP_CONST: r[i->a] = i->imm; break;
      case PET_OP_MOVE: r[i->a] = r[i->b]; break;
      case PET_OP_ADD: wide = (int64_t)r[i->b] + r[i->c]; goto checked;
      case PET_OP_SUB: wide = (int64_t)r[i->b] - r[i->c]; goto checked;
      case PET_OP_MUL: wide = (int64_t)r[i->b] * r[i->c]; goto checked;
      case PET_OP_DIV: case PET_OP_MOD:
        if (!r[i->c]) return fail(w, PET_PROGRAM_DIV_ZERO);
        if (r[i->b] == INT32_MIN && r[i->c] == -1) return fail(w, PET_PROGRAM_OVERFLOW);
        r[i->a] = i->op == PET_OP_DIV ? r[i->b] / r[i->c] : r[i->b] % r[i->c];
        break;
      checked:
        if (wide < INT32_MIN || wide > INT32_MAX) return fail(w, PET_PROGRAM_OVERFLOW);
        r[i->a] = (int32_t)wide; break;
      case PET_OP_LT: r[i->a] = r[i->b] < r[i->c]; break;
      case PET_OP_EQ: r[i->a] = r[i->b] == r[i->c]; break;
      case PET_OP_JUMP: w->pc = (uint16_t)i->imm; break;
      case PET_OP_JZ: if (!r[i->a]) w->pc = (uint16_t)i->imm; break;
      case PET_OP_LOAD:
        if (r[i->b] < 0 || (uint32_t)r[i->b] >= p->cells) return fail(w, PET_PROGRAM_BOUNDS);
        r[i->a] = w->candidate.cells[r[i->b]]; break;
      case PET_OP_STORE:
        if (r[i->a] < 0 || (uint32_t)r[i->a] >= p->cells) return fail(w, PET_PROGRAM_BOUNDS);
        w->candidate.cells[r[i->a]] = r[i->b]; break;
      case PET_OP_EVENT:
        if ((uint32_t)i->imm >= e->count) return fail(w, PET_PROGRAM_BOUNDS);
        r[i->a] = e->words[i->imm]; break;
      case PET_OP_OUTPUT: w->candidate.outputs[i->a] = r[i->b]; break;
#if PET_P4_PROGRAM_V2
      case PET_OP_TEXT:
        r[i->a] = !p->texts ? 0 : r[i->b] < 0 ? 0 : r[i->b] >= p->texts ? p->texts - 1 : r[i->b]; break;
      case PET_OP_CELLGET:
        if (r[i->b] < 0 || r[i->b] >= p->grid_cols || r[i->c] < 0 || r[i->c] >= p->grid_rows) return fail(w, PET_PROGRAM_BOUNDS);
        r[i->a] = w->candidate.cells[(unsigned)r[i->c] * p->grid_cols + (unsigned)r[i->b]]; break;
      case PET_OP_CELLSET:
        if (r[i->a] < 0 || r[i->a] >= p->grid_cols || r[i->b] < 0 || r[i->b] >= p->grid_rows) return fail(w, PET_PROGRAM_BOUNDS);
        w->candidate.cells[(unsigned)r[i->b] * p->grid_cols + (unsigned)r[i->a]] = r[i->c]; break;
#endif
      case PET_OP_INTENT: {
        if (i->a >= a->grant_count || !(a->grants[i->a].operations & (UINT16_C(1) << i->imm)))
          return fail(w, PET_PROGRAM_CAPABILITY);
        if (w->candidate.outbox_count >= p->intent_limit) return fail(w, PET_PROGRAM_CALL_LIMIT);
        pet_program_intent_t *intent = &w->candidate.outbox[w->candidate.outbox_count++];
        intent->capability = a->grants[i->a].id; intent->operation = (uint16_t)i->imm;
        intent->first = r[i->b]; intent->second = r[i->c]; break;
      }
      default: return fail(w, PET_PROGRAM_INVALID);
    }
  }
  // Detect an already-terminal boundary without executing an extra instruction
  // or requiring an empty follow-up slice. Preserve the legacy trap ordering.
  if (w->pc >= p->count) return fail(w, PET_PROGRAM_BAD_PC);
  if (w->used_fuel >= p->fuel) return fail(w, PET_PROGRAM_FUEL_EXHAUSTED);
  return PET_PROGRAM_RUNNING;
}
pet_program_result_t pet_program_step(pet_program_workspace_t *w,const pet_program_authority_t *a,uint16_t max_fuel){
  return pet_program_step_with_checkpoint(w,a,max_fuel,NULL,NULL);
}
pet_program_result_t pet_program_run(const pet_program_t *p, const pet_program_event_t *e,
    const pet_program_state_t *s, const pet_program_authority_t *a, pet_program_workspace_t *w) {
  pet_program_result_t result = pet_program_begin(p, e, s, a, w);
  while (result == PET_PROGRAM_RUNNING) result = pet_program_step(w, a, PET_PROGRAM_QUANTUM);
  return result;
}
bool pet_program_output_contract_valid(const pet_program_output_contract_t *c){
  if(!c)return false;
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)if(c->minimum[i]>c->maximum[i])return false;
  return true;
}
bool pet_program_outputs_fit(const int32_t values[PET_PROGRAM_OUTPUTS],const pet_program_output_contract_t *c){
  if(!values)return false;
  if(!c)return true;
  if(!pet_program_output_contract_valid(c))return false;
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)if(values[i]<c->minimum[i]||values[i]>c->maximum[i])return false;
  return true;
}
static bool contract_overlaps(const void *a,size_t an,const void *b,size_t bn){
  if(!a||!b)return false;
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
pet_program_result_t pet_program_commit_with_publication(pet_program_state_t *s,pet_program_workspace_t *w,
    const pet_program_authority_t *a,const pet_program_output_contract_t *c,pet_program_publish_fn publish,void *context) {
  if (!s || !w) return PET_PROGRAM_INVALID;
  const pet_program_state_t *live=pet_program_state_read(s);
  if(!live)return PET_PROGRAM_INVALID;
  if(c&&(contract_overlaps(c,sizeof(*c),s,sizeof(*s))||contract_overlaps(c,sizeof(*c),w,sizeof(*w))
      ||contract_overlaps(c,sizeof(*c),a,sizeof(*a))))return PET_PROGRAM_INVALID;
  if (!w->ready || w->result != PET_PROGRAM_READY) return PET_PROGRAM_NOT_READY;
  if (w->candidate.outbox_count > PET_PROGRAM_INTENTS) return fail(w, PET_PROGRAM_INVALID);
  if (!current(s, a) || w->candidate.generation != live->generation) return fail(w, PET_PROGRAM_CANCELLED);
  if (live->revision != w->base_revision) return fail(w, PET_PROGRAM_CONFLICT);
  if (live->outbox_count) return fail(w, PET_PROGRAM_OUTBOX_BUSY);
  if (live->revision == UINT32_MAX) return fail(w, PET_PROGRAM_OVERFLOW);
  if(c&&!pet_program_output_contract_valid(c))return fail(w,PET_PROGRAM_INVALID);
  if(!pet_program_outputs_fit(w->candidate.outputs,c))return fail(w,PET_PROGRAM_BOUNDS);
  /* Grants may have been revoked between run and the caller-locked commit. */
  for (unsigned i = 0; i < w->candidate.outbox_count; i++) {
    const pet_program_intent_t *intent = &w->candidate.outbox[i];
    bool permitted = false;
    if (intent->operation < 16) {
      for (unsigned grant = 0; grant < a->grant_count; grant++)
        if (a->grants[grant].id == intent->capability
            && (a->grants[grant].operations & (UINT16_C(1) << intent->operation))) permitted = true;
    }
    if (!permitted) return fail(w, PET_PROGRAM_CAPABILITY);
  }
  w->candidate.revision = live->revision + 1;
  if(publish){if(!publish(context,a->generation,s,&w->candidate))return fail(w,PET_PROGRAM_CANCELLED);}
  else {
#if PET_P4_PROGRAM_V2
    if(s->publication)return fail(w,PET_PROGRAM_INVALID);
#endif
    copy_bytes(s, &w->candidate, sizeof(*s));
  }
  w->ready = false; w->result = PET_PROGRAM_COMMITTED;
  return w->result;
}
pet_program_result_t pet_program_commit_with_contract(pet_program_state_t *s,pet_program_workspace_t *w,
    const pet_program_authority_t *a,const pet_program_output_contract_t *c){
  return pet_program_commit_with_publication(s,w,a,c,NULL,NULL);
}
pet_program_result_t pet_program_commit(pet_program_state_t *s,pet_program_workspace_t *w,
    const pet_program_authority_t *a){return pet_program_commit_with_contract(s,w,a,NULL);}
bool pet_program_ack_outbox(pet_program_state_t *s, uint32_t revision, const pet_program_authority_t *a) {
#if PET_P4_PROGRAM_V2
  if(s&&s->publication)return false; /* Native pure-compute publication has no intents. */
#endif
  if (!current(s, a) || s->revision != revision || s->revision == UINT32_MAX || !s->outbox_count)
    return false;
  zero_bytes(s->outbox, sizeof(s->outbox));
  s->outbox_count = 0;
  s->revision++;
  return true;
}
