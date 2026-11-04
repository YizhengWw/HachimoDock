#include "pet_p4_sfx_core.h"
#include <string.h>
/* Each preset: start/end pitch (Hz), noise mix (0..8 of 8), length (samples),
 * attack (samples), low-pass strength (0..3). Triangle + xorshift noise like the
 * existing fruit cues, so the speaker path and budget stay identical. */
typedef struct { const char *name; uint16_t hz_from, hz_to; uint8_t noise, lowpass; uint16_t attack; uint32_t length; } pet_sfx_preset_t;
static const pet_sfx_preset_t PRESETS[PET_SFX_PRESETS] = {
  {"tap",   900,  600, 3, 0,  40, 960},   /* 60 ms short knock */
  {"beep", 1200, 1200, 0, 0,  80, 1920},  /* 120 ms clean tone */
  {"chirp", 700, 1800, 1, 0,  60, 2880},  /* 180 ms rising chirp */
  {"low",   180,  160, 0, 0, 160, 4800},  /* 300 ms low hum */
  {"water", 400,  300, 7, 3, 120, 3520},  /* 220 ms filtered noise */
  {"page",  500,  500, 6, 1,  20, 1280},  /* 80 ms paper noise */
};
void pet_sfx_reset(pet_sfx_core_t *s) { if (!s) return; memset(s, 0, sizeof(*s)); for (unsigned i = 0; i < PET_SFX_VOICES; i++) s->voices[i].noise = 0x739a42u + i * 0x9e3779b9u; }
const char *pet_sfx_preset_name(unsigned preset) { return preset < PET_SFX_PRESETS ? PRESETS[preset].name : ""; }
int pet_sfx_preset_index(const char *name) {
  if (!name) return -1;
  for (unsigned i = 0; i < PET_SFX_PRESETS; i++) if (!strcmp(PRESETS[i].name, name)) return (int) i;
  return -1;
}
uint32_t pet_sfx_preset_length(unsigned preset) { return preset < PET_SFX_PRESETS ? PRESETS[preset].length : 0; }
bool pet_sfx_trigger(pet_sfx_core_t *s, unsigned preset) {
  if (!s || preset >= PET_SFX_PRESETS) return false;
  pet_sfx_voice_t *v = NULL;
  for (unsigned i = 0; i < PET_SFX_VOICES; i++) if (!s->voices[i].active) { v = &s->voices[i]; break; }
  if (!v) { v = &s->voices[s->next % PET_SFX_VOICES]; s->next++; }
  v->preset = (uint8_t) preset; v->active = true; v->position = 0; v->phase = 0; v->low = 0;
  v->length = PRESETS[preset].length;
  return true;
}
bool pet_sfx_active(const pet_sfx_core_t *s) {
  if (!s) return false;
  for (unsigned i = 0; i < PET_SFX_VOICES; i++) if (s->voices[i].active) return true;
  return false;
}
void pet_sfx_render(pet_sfx_core_t *s, int16_t *pcm, size_t count, unsigned volume) {
  if (!s || !pcm) return;
  if (volume > 100) volume = 100;
  for (size_t i = 0; i < count; i++) {
    int32_t mix = 0;
    for (unsigned k = 0; k < PET_SFX_VOICES; k++) {
      pet_sfx_voice_t *v = &s->voices[k];
      if (!v->active) continue;
      const pet_sfx_preset_t *p = &PRESETS[v->preset];
      if (v->position >= v->length) { v->active = false; continue; }
      v->noise ^= v->noise << 13; v->noise ^= v->noise >> 17; v->noise ^= v->noise << 5;
      uint32_t hz = p->hz_from + (uint32_t) ((int64_t) ((int32_t) p->hz_to - (int32_t) p->hz_from) * (int64_t) v->position / (int64_t) v->length);
      v->phase = (v->phase + hz * 65536u / PET_SFX_RATE) & 65535u;
      int32_t triangle = v->phase < 32768u ? (int32_t) v->phase - 16384 : 49151 - (int32_t) v->phase;
      int32_t n = (int32_t) (v->noise & 32767u) - 16384;
      if (p->lowpass) { v->low = (uint32_t) ((int32_t) v->low * (int32_t) (4 - p->lowpass) + (n + 16384) * (int32_t) p->lowpass) / 4; n = (int32_t) v->low - 16384; }
      int32_t wave = (triangle * (8 - p->noise) + n * p->noise) / 8;
      uint32_t attack = v->position < p->attack ? v->position : p->attack;
      int64_t sample = (int64_t) wave * (int64_t) (v->length - v->position) * (int64_t) attack * (int64_t) volume
        / ((int64_t) v->length * (int64_t) p->attack * 180);
      mix += (int32_t) sample;
      v->position++;
    }
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    pcm[i] = (int16_t) mix;
  }
}

/* ---- p4-sequence-v1: two-track note sequencer ---------------------------- */
/* 440*2^((m-69)/12)*16 for MIDI 36..96, rounded; no float at run time. */
static const uint16_t SEQ_HZ_X16[PET_SEQ_MIDI_MAX - PET_SEQ_MIDI_MIN + 1] = {
  1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976, 2093, 2217, 2349, 2489,
  2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951, 4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272,
  6645, 7040, 7459, 7902, 8372, 8870, 9397, 9956, 10548, 11175, 11840, 12544, 13290, 14080, 14917,
  15804, 16744, 17740, 18795, 19912, 21096, 22351, 23680, 25088, 26580, 28160, 29834, 31609, 33488,
};
uint32_t pet_seq_note_hz_x16(unsigned midi) {
  return midi >= PET_SEQ_MIDI_MIN && midi <= PET_SEQ_MIDI_MAX ? SEQ_HZ_X16[midi - PET_SEQ_MIDI_MIN] : 0;
}
uint32_t pet_seq_samples_per_beat(unsigned bpm) {
  if (bpm < PET_SEQ_BPM_MIN) bpm = PET_SEQ_BPM_MIN;
  if (bpm > PET_SEQ_BPM_MAX) bpm = PET_SEQ_BPM_MAX;
  return 60u * PET_SFX_RATE / bpm;
}
bool pet_seq_program_valid(const pet_seq_program_t *p) {
  if (!p || p->bpm < PET_SEQ_BPM_MIN || p->bpm > PET_SEQ_BPM_MAX || p->track_count < 1 || p->track_count > PET_SEQ_TRACKS) return false;
  for (unsigned t = 0; t < p->track_count; t++) {
    const pet_seq_track_t *track = &p->tracks[t];
    if (track->wave > 2 || track->volume > 100 || track->count < 1 || track->count > PET_SEQ_NOTES) return false;
    for (unsigned i = 0; i < track->count; i++) {
      if (track->midi[i] && (track->midi[i] < PET_SEQ_MIDI_MIN || track->midi[i] > PET_SEQ_MIDI_MAX)) return false;
      if (track->beats[i] < 1 || track->beats[i] > PET_SEQ_BEATS_MAX) return false;
    }
  }
  return true;
}
static void seq_load_note(pet_seq_voice_t *v, const pet_seq_track_t *track, uint32_t spb) {
  unsigned midi = track->midi[v->index];
  v->position = 0; v->phase = 0;
  v->length = spb * track->beats[v->index];
  v->step = midi ? pet_seq_note_hz_x16(midi) * 4096u / PET_SFX_RATE : 0; /* hz*65536/rate with hz_x16 */
}
void pet_seq_start(pet_seq_state_t *s, const pet_seq_program_t *p) {
  if (!s) return;
  memset(s, 0, sizeof(*s));
  if (!pet_seq_program_valid(p)) return;
  uint32_t spb = pet_seq_samples_per_beat(p->bpm);
  for (unsigned t = 0; t < p->track_count; t++) {
    s->voice[t].noise = 0x2545f491u + t * 0x9e3779b9u;
    seq_load_note(&s->voice[t], &p->tracks[t], spb);
  }
  s->playing = true;
}
void pet_seq_stop(pet_seq_state_t *s) { if (s) memset(s, 0, sizeof(*s)); }
bool pet_seq_active(const pet_seq_state_t *s) { return s && s->playing; }
void pet_seq_mix(pet_seq_state_t *s, const pet_seq_program_t *p, int16_t *pcm, size_t count, unsigned volume) {
  if (!s || !pcm || !s->playing) return;
  if (!pet_seq_program_valid(p)) { pet_seq_stop(s); return; }
  if (volume > 100) volume = 100;
  uint32_t spb = pet_seq_samples_per_beat(p->bpm);
  for (size_t i = 0; i < count; i++) {
    int32_t mix = pcm[i];
    bool any = false;
    for (unsigned t = 0; t < p->track_count && t < PET_SEQ_TRACKS; t++) {
      pet_seq_voice_t *v = &s->voice[t];
      const pet_seq_track_t *track = &p->tracks[t];
      if (v->done) continue;
      any = true;
      if (v->step) {
        v->phase = (v->phase + v->step) & 65535u;
        int32_t wave;
        if (track->wave == 0) wave = v->phase < 32768u ? 12000 : -12000;
        else if (track->wave == 1) wave = v->phase < 32768u ? (int32_t) v->phase - 16384 : 49151 - (int32_t) v->phase;
        else { v->noise ^= v->noise << 13; v->noise ^= v->noise >> 17; v->noise ^= v->noise << 5; wave = (int32_t) (v->noise & 32767u) - 16384; }
        /* ADSR-lite: 2 ms attack, 75% sustain after 40 ms, 10 ms release (all integer). */
        uint32_t attack = v->position < 32 ? v->position : 32;
        uint32_t remaining = v->length - v->position;
        uint32_t release = remaining < 160 ? remaining : 160;
        int32_t level = v->position < 640 ? 100 : 75;
        int64_t sample = (int64_t) wave * (int64_t) attack * (int64_t) release * level * (int64_t) track->volume * (int64_t) volume
          / ((int64_t) 32 * 160 * 100 * 100 * 100 * 2);
        mix += (int32_t) sample;
      }
      v->position++;
      if (v->position >= v->length) {
        v->index++;
        if (v->index >= track->count) {
          if (p->loop) v->index = 0; else { v->done = true; continue; }
        }
        seq_load_note(v, track, spb);
      }
    }
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    pcm[i] = (int16_t) mix;
    bool all_done = true;
    for (unsigned t = 0; t < p->track_count && t < PET_SEQ_TRACKS; t++) if (!s->voice[t].done) all_done = false;
    if (!any || all_done) { s->playing = false; return; }
  }
}

/* ---- p4-samples-v1: two one-shot PCM voices ------------------------------- */
void pet_sampler_reset(pet_sampler_t *s) { if (s) memset(s, 0, sizeof(*s)); }
bool pet_sampler_trigger(pet_sampler_t *s, const int16_t *pcm, uint32_t frames) {
  if (!s || !pcm || !frames || frames > PET_SAMPLE_MAX_BYTES / sizeof(*pcm)) return false;
  pet_sample_voice_t *v = NULL;
  for (unsigned i = 0; i < PET_SAMPLE_VOICES; i++) if (!s->voice[i].active) { v = &s->voice[i]; break; }
  if (!v) { v = &s->voice[s->next % PET_SAMPLE_VOICES]; s->next++; }
  v->pcm = pcm; v->frames = frames; v->position = 0; v->active = true;
  return true;
}
bool pet_sampler_active(const pet_sampler_t *s) {
  if (!s) return false;
  for (unsigned i = 0; i < PET_SAMPLE_VOICES; i++) if (s->voice[i].active) return true;
  return false;
}
void pet_sampler_mix(pet_sampler_t *s, int16_t *pcm, size_t count, unsigned volume) {
  if (!s || !pcm) return;
  if (volume > 100) volume = 100;
  for (size_t i = 0; i < count; i++) {
    int32_t mix = pcm[i];
    for (unsigned k = 0; k < PET_SAMPLE_VOICES; k++) {
      pet_sample_voice_t *v = &s->voice[k];
      if (!v->active) continue;
      if (v->position >= v->frames) { v->active = false; v->pcm = NULL; continue; }
      mix += (int32_t) v->pcm[v->position++] * (int32_t) volume / 100;
      if (v->position == v->frames) { v->active = false; v->pcm = NULL; }
    }
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    pcm[i] = (int16_t) mix;
  }
}
