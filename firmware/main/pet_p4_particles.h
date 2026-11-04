/* p4-particles-v1: decorative fixed-step Q8 dot emitters. Pure integer core shared
 * by the device renderer and the preview WASM; no allocation, floats, clocks, IO
 * or collisions. Dots live inside one rectangle and never move surface nodes. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PET_PARTICLES_MAX_DOTS 64U
#define PET_PARTICLES_STEP_MS 20U
#define PET_PARTICLES_MAX_STEPS 8U
#define PET_PARTICLES_DEAD 0xFFFFU
enum {PET_PARTICLES_DOT=0,PET_PARTICLES_SPARK=1};
typedef struct {
  uint16_t x,y,w,h,color,life_ms,tick_ms,spread; /* tick_ms 0 = action-triggered burst only */
  uint8_t count,budget,speed,shape;              /* speed px/s, budget = dots drawn per frame */
  int16_t gravity;                                /* px/s^2, positive pulls down */
} pet_particles_config_t;
typedef struct {
  int32_t x[PET_PARTICLES_MAX_DOTS],y[PET_PARTICLES_MAX_DOTS],vx[PET_PARTICLES_MAX_DOTS],vy[PET_PARTICLES_MAX_DOTS]; /* Q8 px, Q8 px/s */
  uint16_t age[PET_PARTICLES_MAX_DOTS];           /* ms, PET_PARTICLES_DEAD when free */
  uint32_t rng,tick_accum;
  uint64_t last_ms;
  bool clocked;
} pet_particles_state_t;
typedef void (*pet_particles_dot_fn)(void *context,int x,int y,int size,uint16_t color);
bool pet_particles_valid(const pet_particles_config_t *c);
void pet_particles_reset(pet_particles_state_t *s,uint32_t seed);
/* Bhaskara integer sine, degrees in, Q8 out; shared so previews match the board. */
int32_t pet_particles_sin_q8(int degrees);
/* Spawn up to count-alive dots at once (action trigger). Returns spawned dots. */
unsigned pet_particles_burst(const pet_particles_config_t *c,pet_particles_state_t *s);
/* Advance in 20 ms steps (at most 8 per call); emitting enables tick spawns. Returns alive dots. */
unsigned pet_particles_step(const pet_particles_config_t *c,pet_particles_state_t *s,uint64_t now_ms,bool emitting);
/* Emit at most budget dots (size 1 or 3) in slot order; callback clips. Returns emitted dots. */
unsigned pet_particles_draw(const pet_particles_config_t *c,const pet_particles_state_t *s,pet_particles_dot_fn dot,void *context);
unsigned pet_particles_alive(const pet_particles_state_t *s);
