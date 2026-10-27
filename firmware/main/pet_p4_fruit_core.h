#pragma once
#include "pet_p4_touch_core.h"
#define PET_FRUIT_MAX 8
#define PET_FRUIT_PARTICLES 40
#define PET_FRUIT_TRAIL 24
typedef enum { PET_FRUIT_IDLE, PET_FRUIT_PLAYING, PET_FRUIT_PAUSED, PET_FRUIT_RESULT } pet_fruit_phase_t;
typedef enum { PET_FRUIT_CLASSIC, PET_FRUIT_RUSH, PET_FRUIT_ZEN } pet_fruit_mode_t;
typedef struct {
  bool active, cut, bomb, golden;
  unsigned kind;
  float x,y,vx,vy,angle,spin,radius;
  uint32_t cut_age;
} pet_fruit_item_t;
typedef struct {float x,y,vx,vy; unsigned life,kind;} pet_fruit_particle_t;
typedef struct {int x,y; uint64_t at; bool start;} pet_fruit_trail_t;
typedef struct {
  pet_fruit_phase_t phase;
  unsigned duration_sec,lives,sound_volume,score,best,misses,combo,max_combo,cut_count;
  unsigned elapsed_ms,spawn_ms,flash_ms,combo_ms,feedback_combo,sounds;
  pet_fruit_mode_t mode;
  unsigned records[3],wave,level,energy,fever_ms,feedback_ms,feedback_points,feedback_kind;
  float feedback_x,feedback_y;
  uint64_t last_cut_ms;
  bool menu_down,pause_down;
  uint64_t menu_at;
  int menu_x,menu_y;
  uint32_t rng;
  uint64_t now_ms,stroke_ms;
  bool down,bomb_hit;
  int px,py;
  float previous_x[PET_FRUIT_MAX],previous_y[PET_FRUIT_MAX];
  unsigned trail_count;
  pet_fruit_trail_t trail[PET_FRUIT_TRAIL];
  pet_fruit_item_t fruit[PET_FRUIT_MAX];
  pet_fruit_particle_t particles[PET_FRUIT_PARTICLES];
} pet_fruit_t;
void pet_fruit_init(pet_fruit_t *s,unsigned seconds,unsigned lives,unsigned volume);
void pet_fruit_start(pet_fruit_t *s,uint64_t now,uint32_t seed);
/* Physical start must not discard an active round. */
void pet_fruit_start_or_resume(pet_fruit_t *s,uint64_t now,uint32_t seed);
void pet_fruit_pause(pet_fruit_t *s,uint64_t now);
void pet_fruit_select_mode(pet_fruit_t *s,unsigned mode);
void pet_fruit_suspend(pet_fruit_t *s,uint64_t now);
void pet_fruit_tick(pet_fruit_t *s,uint64_t now);
void pet_fruit_pointer(pet_fruit_t *s,const pet_p4_pointer_event_t *e);
