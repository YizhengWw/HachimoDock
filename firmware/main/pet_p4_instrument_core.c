#include "pet_p4_instrument_core.h"
#include <math.h>
#include <string.h>
#include "pet_p4_instrument_tone_table.h"
#define RATE 16000.0f
#define TAU 6.28318530718f
void pet_instrument_reset(pet_instrument_core_t *s) {
  memset(s,0,sizeof(*s)); s->noise=0x6d757975;
  for(int i=0;i<4;i++)s->age[i]=16000;
}
void pet_instrument_strike(pet_instrument_core_t *s) {
  s->age[s->next++%4]=0;
}
pet_instrument_motion_t pet_instrument_motion(uint64_t ms) {
  return pet_instrument_motion_from(ms, 0, 1);
}
pet_instrument_motion_t pet_instrument_motion_from(uint64_t ms,float start_y,float start_scale) {
  if(ms>=900)return (pet_instrument_motion_t){0,1,1};
  if(ms<55) {float u=ms/55.0f;return (pet_instrument_motion_t){start_y+(68-start_y)*u*u,start_scale+(1-start_scale)*u,1};}
  float t=(ms-55)/1000.0f;
  /* 55 ms descent, contact and sound together, then damped rebound. */
  return (pet_instrument_motion_t){68*expf(-8*t)*cosf(16*t),
    1-0.026f*expf(-10*t)*cosf(23*t),fminf(t/0.85f,1)};
}
void pet_instrument_render(pet_instrument_core_t *s,int16_t *out,size_t n,
                          bool enabled,unsigned effect,unsigned ambience) {
  pet_instrument_render_tone(s,out,n,enabled,effect,ambience,PET_INSTRUMENT_DEFAULT_TONE);
}
void pet_instrument_render_tone(pet_instrument_core_t *s,int16_t *out,size_t n,
                               bool enabled,unsigned effect,unsigned ambience,unsigned tone) {
  static const float notes[]={261.6256f,329.6276f,391.9954f,440.0f,391.9954f,329.6276f,293.6648f,329.6276f};
  /* Inharmonic resonances and a brief noisy impact: wood, not a sine-wave beep. */
  static const float tuning[3][11]={
    {1050,1808,2780, .44f,.28f,.15f, 28,48,72, .12f,340},
    {690,1173,1820, .54f,.28f,.12f, 12,21,42, .08f,250},
    {440,817,1327, .64f,.23f,.09f, 22,34,55, .045f,200}
  };
  const float *p=tuning[tone<3?tone:0];
  effect=effect>100?100:effect; ambience=ambience>100?100:ambience;
  for(size_t i=0;i<n;i++) {
    float target=enabled?1:0;
    s->gain += fmaxf(-0.0007f,fminf(0.00012f,target-s->gain));
    float hit=0,duck=1;
    s->noise^=s->noise<<13;s->noise^=s->noise>>17;s->noise^=s->noise<<5;
    for(int v=0;v<4;v++)if(s->age[v]<16000) {
      uint32_t age=s->age[v]++;
      if(age<880)continue;
      float t=(age-880)/RATE;
      float attack=fminf(t*2200,1);
      if(tone==PET_INSTRUMENT_DEFAULT_TONE) {
        /* Flash PCM removes 24 transcendental operations/sample with four
         * overlapping hits. Mixing, volume and the limiter remain live. */
        if(age<PET_INSTRUMENT_TONE_SAMPLES)hit+=pet_instrument_tone_pcm[age]/32768.0f;
      } else hit+=attack*(p[3]*sinf(TAU*p[0]*t)*expf(-p[6]*t)
          +p[4]*sinf(TAU*p[1]*t)*expf(-p[7]*t)
          +p[5]*sinf(TAU*p[2]*t)*expf(-p[8]*t)
          +p[9]*((int32_t)(s->noise&65535)-32768)/32768.0f*expf(-p[10]*t));
      if(t<0.28f)duck=0.5f+0.5f*t/0.28f;
    }
    /* Original pentatonic plucks. Eight 2-second notes, bounded phase clock. */
    uint32_t clock=s->samples++%256000;
    float t=(clock%32000)/RATE,f=notes[clock/32000];
    float music=(sinf(TAU*f*t)+0.18f*sinf(TAU*f*2*t))
      *fminf(t*8,1)*expf(-1.8f*t);
    /* The 18% ambience setting already attenuates the bed. Avoid a second
     * excessive attenuation that made it effectively inaudible on laptops. */
    float mixed=(hit*effect/100.0f+music*ambience/100.0f*0.80f*duck)*s->gain;
    /* Soft limiter bounds four overlapping strikes without integer wrap. */
    float limited=27000.0f*mixed/(1+fabsf(mixed)*0.5f);
    out[i]=(int16_t)fmaxf(-32000,fminf(32000,limited));
  }
}
