#include "pet_p4_particles.h"
bool pet_particles_valid(const pet_particles_config_t *c){
  return c&&c->w&&c->h&&c->x+c->w<=640&&c->y+c->h<=480&&c->count&&c->count<=PET_PARTICLES_MAX_DOTS
    &&c->budget&&c->budget<=c->count&&c->life_ms>=100&&c->life_ms<=4000&&c->speed&&c->speed<=240&&c->spread<=360
    &&c->gravity>=-240&&c->gravity<=240&&(c->shape==PET_PARTICLES_DOT||c->shape==PET_PARTICLES_SPARK)
    &&(!c->tick_ms||(c->tick_ms>=50&&c->tick_ms<=2000));
}
void pet_particles_reset(pet_particles_state_t *s,uint32_t seed){
  if(!s)return;
  for(unsigned i=0;i<PET_PARTICLES_MAX_DOTS;i++){s->x[i]=s->y[i]=s->vx[i]=s->vy[i]=0;s->age[i]=PET_PARTICLES_DEAD;}
  s->rng=seed?seed:0x70617274U;s->tick_accum=0;s->last_ms=0;s->clocked=false;
}
static uint32_t next_random(pet_particles_state_t *s){s->rng=s->rng*1664525U+1013904223U;return s->rng>>8;}
/* Truncating division like C on both sides of the parity test. */
static int32_t div_trunc(int64_t value,int32_t divisor){return (int32_t)(value/divisor);}
int32_t pet_particles_sin_q8(int degrees){
  int d=degrees%360;if(d<0)d+=360;
  bool negative=d>=180;if(negative)d-=180;
  int32_t p=d*(180-d);           /* 0..8100 */
  int32_t value=(16*p*256)/(40500-p);
  return negative?-value:value;
}
unsigned pet_particles_alive(const pet_particles_state_t *s){
  unsigned n=0;if(!s)return 0;
  for(unsigned i=0;i<PET_PARTICLES_MAX_DOTS;i++)if(s->age[i]!=PET_PARTICLES_DEAD)n++;
  return n;
}
static bool spawn(const pet_particles_config_t *c,pet_particles_state_t *s){
  for(unsigned i=0;i<c->count;i++){
    if(s->age[i]!=PET_PARTICLES_DEAD)continue;
    uint32_t rx=next_random(s),ry=next_random(s),ra=next_random(s);
    s->x[i]=(int32_t)((c->x+rx%c->w)*256U+128U);
    s->y[i]=(int32_t)((c->y+ry%c->h)*256U+128U);
    /* Cone around straight up (270 degrees on screen): -spread/2..+spread/2. */
    int degrees=270-(int)c->spread/2+(c->spread?(int)(ra%(c->spread+1U)):0);
    s->vx[i]=div_trunc((int64_t)c->speed*pet_particles_sin_q8(degrees+90),1);
    s->vy[i]=div_trunc((int64_t)c->speed*pet_particles_sin_q8(degrees),1);
    s->age[i]=0;return true;
  }
  return false;
}
unsigned pet_particles_burst(const pet_particles_config_t *c,pet_particles_state_t *s){
  if(!pet_particles_valid(c)||!s)return 0;
  unsigned spawned=0;while(spawned<c->count&&spawn(c,s))spawned++;
  return spawned;
}
static void step_once(const pet_particles_config_t *c,pet_particles_state_t *s,bool emitting){
  int32_t right=(int32_t)(c->x+c->w)*256,bottom=(int32_t)(c->y+c->h)*256,left=(int32_t)c->x*256,top=(int32_t)c->y*256;
  for(unsigned i=0;i<c->count;i++){
    if(s->age[i]==PET_PARTICLES_DEAD)continue;
    s->vy[i]+=div_trunc((int64_t)c->gravity*256*PET_PARTICLES_STEP_MS,1000);
    s->x[i]+=div_trunc((int64_t)s->vx[i]*PET_PARTICLES_STEP_MS,1000);
    s->y[i]+=div_trunc((int64_t)s->vy[i]*PET_PARTICLES_STEP_MS,1000);
    uint32_t age=(uint32_t)s->age[i]+PET_PARTICLES_STEP_MS;
    if(age>=c->life_ms||s->x[i]<left||s->x[i]>=right||s->y[i]<top||s->y[i]>=bottom)s->age[i]=PET_PARTICLES_DEAD;
    else s->age[i]=(uint16_t)age;
  }
  if(emitting&&c->tick_ms){
    s->tick_accum+=PET_PARTICLES_STEP_MS;
    while(s->tick_accum>=c->tick_ms){s->tick_accum-=c->tick_ms;(void)spawn(c,s);}
  }
}
unsigned pet_particles_step(const pet_particles_config_t *c,pet_particles_state_t *s,uint64_t now_ms,bool emitting){
  if(!pet_particles_valid(c)||!s)return 0;
  if(!s->clocked||now_ms<s->last_ms){s->last_ms=now_ms;s->clocked=true;return pet_particles_alive(s);}
  uint64_t elapsed=now_ms-s->last_ms;unsigned steps=(unsigned)(elapsed/PET_PARTICLES_STEP_MS);
  if(steps>PET_PARTICLES_MAX_STEPS){steps=PET_PARTICLES_MAX_STEPS;s->last_ms=now_ms-(uint64_t)steps*PET_PARTICLES_STEP_MS;}
  for(unsigned i=0;i<steps;i++)step_once(c,s,emitting);
  s->last_ms+=(uint64_t)steps*PET_PARTICLES_STEP_MS;
  return pet_particles_alive(s);
}
unsigned pet_particles_draw(const pet_particles_config_t *c,const pet_particles_state_t *s,pet_particles_dot_fn dot,void *context){
  if(!pet_particles_valid(c)||!s||!dot)return 0;
  unsigned emitted=0;
  for(unsigned i=0;i<c->count&&emitted<c->budget;i++){
    if(s->age[i]==PET_PARTICLES_DEAD)continue;
    dot(context,s->x[i]>>8,s->y[i]>>8,c->shape==PET_PARTICLES_SPARK?3:1,c->color);emitted++;
  }
  return emitted;
}
