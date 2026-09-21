#pragma once
#include "pet_p4_protocol.h"

bool pet_p4_conversation_active(const pet_p4_runtime_state_t *state);
void pet_p4_conversation_clear(pet_p4_runtime_state_t *state);
bool pet_p4_conversation_update(pet_p4_runtime_state_t *state, const char *id,
  const char *phase, const char *name, const char *text, bool user, bool final,
  unsigned long long now_ms);
void pet_p4_conversation_move(pet_p4_runtime_state_t *state, int direction, unsigned long long now_ms);
void pet_p4_conversation_playback_begin(pet_p4_runtime_state_t *state, const char *id);
bool pet_p4_conversation_queue_cue(pet_p4_runtime_state_t *state, const char *id,
  unsigned int offset_bytes, const char *text);
void pet_p4_conversation_advance(pet_p4_runtime_state_t *state, unsigned int played_bytes,
  bool playing, unsigned long long now_ms);
