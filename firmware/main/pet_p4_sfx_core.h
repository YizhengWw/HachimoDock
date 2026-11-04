#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* p4-sfx-v1: bounded component sound cues. Portable DSP owned by the speaker worker;
 * no allocation, no files, no transcendentals per sample. Six fixed presets, two
 * simultaneous voices, 16 kHz mono. Packages pick presets by name, never samples. */
#define PET_SFX_VOICES 2u
#define PET_SFX_PRESETS 6u
#define PET_SFX_MAX_CUES 8u
#define PET_SFX_RATE 16000u
typedef struct {
  uint8_t preset;
  bool active;
  uint32_t position, length, phase, noise, low;
} pet_sfx_voice_t;
typedef struct {
  pet_sfx_voice_t voices[PET_SFX_VOICES];
  unsigned next;
} pet_sfx_core_t;
void pet_sfx_reset(pet_sfx_core_t *s);
/* Starts a preset on a free voice, or replaces the oldest one. False for an unknown preset. */
bool pet_sfx_trigger(pet_sfx_core_t *s, unsigned preset);
bool pet_sfx_active(const pet_sfx_core_t *s);
/* Mixes active voices into count samples at volume 0..100, clamped to int16. */
void pet_sfx_render(pet_sfx_core_t *s, int16_t *pcm, size_t count, unsigned volume);
const char *pet_sfx_preset_name(unsigned preset);
int pet_sfx_preset_index(const char *name);
/* Length of a preset in samples, for callers that bound their own timing. */
uint32_t pet_sfx_preset_length(unsigned preset);

/* p4-sequence-v1 / p4-samples-v1 (Track C, 2026-11-02). Pure DSP that the
 * speaker worker mixes on top of the sfx voices through the same gated
 * sfx_drain_step; compiled always (no behaviour without a caller), enabled on
 * device only by PET_P4_SEQUENCE / PET_P4_SAMPLES. No allocation, no files,
 * no transcendentals: one 61-entry note table (MIDI 36..96), integer phase
 * accumulators, an ADSR-lite envelope and a hard clamp. */
#define PET_SEQ_TRACKS 2u
#define PET_SEQ_NOTES 64u
#define PET_SEQ_MIDI_MIN 36u
#define PET_SEQ_MIDI_MAX 96u
#define PET_SEQ_BPM_MIN 40u
#define PET_SEQ_BPM_MAX 240u
#define PET_SEQ_BEATS_MAX 16u
typedef struct {
  uint8_t wave;   /* 0 square, 1 triangle, 2 noise */
  uint8_t volume; /* 0..100 track gain */
  uint8_t count;  /* 1..64 notes */
  uint8_t midi[PET_SEQ_NOTES];  /* 36..96, 0 = rest */
  uint8_t beats[PET_SEQ_NOTES]; /* 1..16 beats */
} pet_seq_track_t;
typedef struct {
  uint16_t bpm;
  uint8_t track_count; /* 1..2 */
  bool loop;
  pet_seq_track_t tracks[PET_SEQ_TRACKS];
} pet_seq_program_t;
typedef struct {
  uint8_t index;
  bool done;
  uint32_t position, length, phase, step, noise;
} pet_seq_voice_t;
typedef struct {
  bool playing;
  pet_seq_voice_t voice[PET_SEQ_TRACKS];
} pet_seq_state_t;
/* Validates the program shape (bpm 40-240, 1-2 tracks, 1-64 notes each, midi 36-96
 * or rest, beats 1-16, waves 0-2, volume <= 100). Parsers call this after filling. */
bool pet_seq_program_valid(const pet_seq_program_t *program);
void pet_seq_start(pet_seq_state_t *state, const pet_seq_program_t *program);
void pet_seq_stop(pet_seq_state_t *state);
bool pet_seq_active(const pet_seq_state_t *state);
/* Adds the sequencer output to count samples already in pcm (saturating). */
void pet_seq_mix(pet_seq_state_t *state, const pet_seq_program_t *program, int16_t *pcm, size_t count, unsigned volume);
/* Note period helpers for tests and schedulers. */
uint32_t pet_seq_note_hz_x16(unsigned midi);
uint32_t pet_seq_samples_per_beat(unsigned bpm);

#define PET_SAMPLE_VOICES 2u
#define PET_SAMPLE_MAX_SLOTS 4u
#define PET_SAMPLE_MAX_BYTES (64u * 1024u)
#define PET_SAMPLE_BUDGET_BYTES (256u * 1024u)
typedef struct {
  const int16_t *pcm;
  uint32_t frames, position;
  bool active;
} pet_sample_voice_t;
typedef struct {
  pet_sample_voice_t voice[PET_SAMPLE_VOICES];
  unsigned next;
} pet_sampler_t;
void pet_sampler_reset(pet_sampler_t *s);
/* Starts a one-shot on a free voice or replaces the oldest; false for an empty sample. */
bool pet_sampler_trigger(pet_sampler_t *s, const int16_t *pcm, uint32_t frames);
bool pet_sampler_active(const pet_sampler_t *s);
void pet_sampler_mix(pet_sampler_t *s, int16_t *pcm, size_t count, unsigned volume);
