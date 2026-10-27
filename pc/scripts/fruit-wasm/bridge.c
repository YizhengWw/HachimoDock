/* Trusted desktop runtime compiled from the same bounded C core as firmware.
 * Never loaded from a component package. No WASI, files, network or imports. */
#include "pet_p4_fruit_core.h"
#include <stddef.h>
void *memset(void *p,int v,size_t n){unsigned char *d=p;while(n--)*d++=(unsigned char)v;return p;}
void *memcpy(void *p,const void *q,size_t n){unsigned char*d=p;const unsigned char*s=q;while(n--)*d++=*s++;return p;}
void *memmove(void *p,const void *q,size_t n){unsigned char*d=p;const unsigned char*s=q;if(d<s)while(n--)*d++=*s++;else {d+=n;s+=n;while(n--)*--d=*--s;}return p;}
static pet_fruit_t game;
static float snapshot[512];
void fruit_init(unsigned seconds,unsigned lives,unsigned volume){pet_fruit_init(&game,seconds,lives,volume);}
void fruit_action(unsigned action,unsigned now,unsigned seed){if(!action)pet_fruit_start(&game,now,seed);else pet_fruit_pause(&game,now);}
void fruit_tick(unsigned now){pet_fruit_tick(&game,now);}
void fruit_pointer(unsigned kind,int x,int y,unsigned now){pet_p4_pointer_event_t e={kind,x,y,now};pet_fruit_pointer(&game,&e);}
void fruit_suspend(unsigned now){pet_fruit_suspend(&game,now);}
void fruit_mode(unsigned mode){pet_fruit_select_mode(&game,mode);}
unsigned fruit_sound(void){unsigned v=game.sounds;game.sounds=0;return v;}
float *fruit_snapshot(void){
  unsigned at=0;
#define PUSH(v) snapshot[at++]=(float)(v)
  PUSH(game.phase);PUSH(game.score);PUSH(game.best);PUSH(game.misses);PUSH(game.lives);PUSH(game.duration_sec);
  PUSH(game.elapsed_ms);PUSH(game.feedback_combo);PUSH(game.max_combo);PUSH(game.combo_ms);PUSH(game.bomb_hit);PUSH(game.down);
  PUSH(game.mode);PUSH(game.level);PUSH(game.energy);PUSH(game.fever_ms);PUSH(game.feedback_ms);PUSH(game.feedback_points);PUSH(game.feedback_kind);PUSH(game.feedback_x);PUSH(game.feedback_y);PUSH(game.cut_count);
  for(unsigned i=0;i<PET_FRUIT_MAX;i++){pet_fruit_item_t*f=&game.fruit[i];PUSH(f->active);PUSH(f->cut);PUSH(f->bomb);PUSH(f->kind);PUSH(f->x);PUSH(f->y);PUSH(f->angle);PUSH(f->cut_age);PUSH(f->radius);PUSH(f->golden);}
  for(unsigned i=0;i<PET_FRUIT_PARTICLES;i++){pet_fruit_particle_t*p=&game.particles[i];PUSH(p->x);PUSH(p->y);PUSH(p->life);PUSH(p->kind);}
  PUSH(game.trail_count);
  for(unsigned i=0;i<game.trail_count;i++){PUSH(game.trail[i].x);PUSH(game.trail[i].y);PUSH(game.trail[i].start);}
  return snapshot;
}
