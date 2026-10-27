#include "pet_p4_fruit_core.h"
#include <string.h>

static uint32_t random_next(pet_fruit_t *s) {
  uint32_t x=s->rng?s->rng:0x1234567;
  x^=x<<13;x^=x>>17;x^=x<<5;return s->rng=x;
}
void pet_fruit_init(pet_fruit_t *s,unsigned seconds,unsigned lives,unsigned volume) {
  memset(s,0,sizeof(*s));s->duration_sec=seconds;s->lives=lives;s->sound_volume=volume;
}
void pet_fruit_select_mode(pet_fruit_t *s,unsigned mode) {
  if(mode>2 || (s->phase!=PET_FRUIT_IDLE&&s->phase!=PET_FRUIT_RESULT))return;
  s->mode=(pet_fruit_mode_t)mode;s->best=s->records[mode];
  s->phase=PET_FRUIT_IDLE;s->score=0;s->elapsed_ms=0;s->misses=0;
  s->down=s->menu_down=s->pause_down=false;s->trail_count=0;s->sounds=0;
  memset(s->fruit,0,sizeof(s->fruit));memset(s->particles,0,sizeof(s->particles));
  s->feedback_ms=s->combo_ms=s->fever_ms=s->energy=0;
}
void pet_fruit_start(pet_fruit_t *s,uint64_t now,uint32_t seed) {
  unsigned seconds=s->duration_sec,lives=s->lives,volume=s->sound_volume;
  unsigned records[3];memcpy(records,s->records,sizeof(records));
  pet_fruit_mode_t mode=s->mode;
  pet_fruit_init(s,seconds,lives,volume);s->mode=mode;
  memcpy(s->records,records,sizeof(records));s->best=records[mode];
  s->now_ms=now;s->rng=seed;s->phase=PET_FRUIT_PLAYING;s->spawn_ms=350;s->level=1;
}
void pet_fruit_suspend(pet_fruit_t *s,uint64_t now) {
  if(s->phase==PET_FRUIT_PLAYING)s->phase=PET_FRUIT_PAUSED;
  s->down=s->menu_down=s->pause_down=false;s->combo=0;s->trail_count=0;s->now_ms=now;s->sounds=0;
}
void pet_fruit_start_or_resume(pet_fruit_t *s,uint64_t now,uint32_t seed) {
  if(s->phase==PET_FRUIT_PAUSED)pet_fruit_pause(s,now);
  else if(s->phase!=PET_FRUIT_PLAYING)pet_fruit_start(s,now,seed);
}
void pet_fruit_pause(pet_fruit_t *s,uint64_t now) {
  if(s->phase==PET_FRUIT_PLAYING)pet_fruit_suspend(s,now);
  else if(s->phase==PET_FRUIT_PAUSED){s->phase=PET_FRUIT_PLAYING;s->now_ms=now;s->down=s->menu_down=s->pause_down=false;}
  else pet_fruit_select_mode(s,(s->mode+1)%3);
}
static void finish(pet_fruit_t *s,bool bomb) {
  s->phase=PET_FRUIT_RESULT;s->bomb_hit=bomb;s->down=s->menu_down=s->pause_down=false;s->flash_ms=bomb?300:0;
  if(s->score>s->records[s->mode])s->records[s->mode]=s->score;
  s->best=s->records[s->mode];s->sounds|=bomb?2:4;
}
static void spawn(pet_fruit_t *s) {
  s->wave++;
  unsigned count=2+random_next(s)%3+(s->mode==PET_FRUIT_RUSH&&s->level>=3?1:0);
  bool has_bomb=s->mode!=PET_FRUIT_ZEN && s->elapsed_ms>5000 && random_next(s)%4==0;
  bool has_gold=s->wave%5==0;
  const float radii[4]={26,23,20,34};
  unsigned formation=s->wave%3;
  float origin=150+random_next(s)%341;
  for(unsigned i=0,n=0;i<PET_FRUIT_MAX && count;i++)if(!s->fruit[i].active){
    pet_fruit_item_t *f=&s->fruit[i];memset(f,0,sizeof(*f));f->active=true;
    f->bomb=has_bomb;has_bomb=false;f->kind=random_next(s)%4;
    f->golden=!f->bomb&&has_gold;if(f->golden)has_gold=false;
    f->radius=f->bomb?27:radii[f->kind]+(int)(random_next(s)%9)-4;
    f->x=formation==0?origin+(int)n*55-70:80+random_next(s)%481;
    if(f->x>560)f->x=560;
    if(f->x<80)f->x=80;
    f->y=525+random_next(s)%35;
    f->vx=formation==2?(n%2?100:-100):(320-f->x)*.26f+(int)(random_next(s)%61)-30;
    f->vy=-560-(int)(random_next(s)%131)-(int)s->level*5;
    f->spin=(int)(random_next(s)%201)-100;f->angle=(int)(random_next(s)%41)-20;
    s->previous_x[i]=f->x;s->previous_y[i]=f->y;count--;n++;
  }
}
static unsigned decay(unsigned value,unsigned delta){return value>delta?value-delta:0;}
void pet_fruit_tick(pet_fruit_t *s,uint64_t now) {
  if(now<s->now_ms){s->down=s->menu_down=false;s->trail_count=0;return;}
  uint64_t delta=now-s->now_ms;s->now_ms=now;
  unsigned keep=0;
  for(unsigned i=0;i<s->trail_count;i++)if(now>=s->trail[i].at&&now-s->trail[i].at<180)s->trail[keep++]=s->trail[i];
  s->trail_count=keep;
  if(s->down&&now-s->stroke_ms>150){s->down=false;s->combo=0;}
  if(s->phase!=PET_FRUIT_PLAYING)return;
  if(delta>250){pet_fruit_suspend(s,now);return;}
  s->elapsed_ms+=(unsigned)delta;
  if(s->elapsed_ms>=s->duration_sec*1000){finish(s,false);return;}
  s->level=1+s->elapsed_ms/12000;if(s->level>5)s->level=5;
  s->combo_ms=decay(s->combo_ms,delta);s->feedback_ms=decay(s->feedback_ms,delta);
  s->fever_ms=decay(s->fever_ms,delta);s->flash_ms=decay(s->flash_ms,delta);
  if(s->spawn_ms>delta)s->spawn_ms-=(unsigned)delta;
  else {
    spawn(s);
    s->spawn_ms=(s->mode==PET_FRUIT_ZEN?1250:s->mode==PET_FRUIT_RUSH?1050:1450)-s->level*90+random_next(s)%250;
    if(s->fever_ms)s->spawn_ms=600+random_next(s)%150;
  }
  while(delta){
    unsigned ms=delta>8?8:(unsigned)delta;delta-=ms;float dt=ms/1000.f;
    for(unsigned i=0;i<PET_FRUIT_MAX;i++){
      pet_fruit_item_t *f=&s->fruit[i];if(!f->active)continue;
      f->x+=f->vx*dt;f->y+=f->vy*dt+310*dt*dt;f->vy+=620*dt;f->angle+=f->spin*dt;
      if(f->cut)f->cut_age+=ms;
      if((f->cut&&f->cut_age>650)||f->y>590){
        if(!f->cut&&!f->bomb){
          s->misses++;
          if(s->mode==PET_FRUIT_CLASSIC&&s->misses>=s->lives){finish(s,false);return;}
        }
        f->active=false;
      }
    }
    for(unsigned i=0;i<PET_FRUIT_PARTICLES;i++){
      pet_fruit_particle_t *p=&s->particles[i];if(!p->life)continue;
      p->life=decay(p->life,ms);p->x+=p->vx*dt;p->y+=p->vy*dt;p->vy+=480*dt;
    }
  }
}
static bool hit(float ax,float ay,float bx,float by,float radius) {
  float dx=bx-ax,dy=by-ay,d=dx*dx+dy*dy;
  float t=d>0?-(ax*dx+ay*dy)/d:0;if(t<0)t=0;if(t>1)t=1;
  float x=ax+t*dx,y=ay+t*dy;return x*x+y*y<=radius*radius;
}
static bool menu_pointer(pet_fruit_t *s,const pet_p4_pointer_event_t *e) {
  if(s->phase==PET_FRUIT_PLAYING)return false;
  if(e->kind==PET_P4_POINTER_DOWN){s->menu_down=true;s->menu_x=e->x;s->menu_y=e->y;}
  else if(e->kind==PET_P4_POINTER_CANCEL)s->menu_down=false;
  else if(e->kind==PET_P4_POINTER_MOVE){
    int dx=e->x-s->menu_x,dy=e->y-s->menu_y;
    if(dx*dx+dy*dy>144)s->menu_down=false;
  }
  else if(e->kind==PET_P4_POINTER_UP){
    int dx=e->x-s->menu_x,dy=e->y-s->menu_y;
    bool tap=s->menu_down&&dx*dx+dy*dy<=144;s->menu_down=false;
    if(tap){
      if(s->phase==PET_FRUIT_PAUSED){
        if(e->x>=190&&e->x<=450&&e->y>=280&&e->y<=338)pet_fruit_pause(s,e->ts_ms);
      } else if(e->y>=302&&e->y<=374&&e->x>=32&&e->x<608){
        pet_fruit_select_mode(s,(unsigned)(e->x-32)/192);
      } else if(e->y>=399&&e->y<=455&&e->x>=185&&e->x<=455){
        pet_fruit_start(s,e->ts_ms,(uint32_t)e->ts_ms);
      }
    }
  }
  return true;
}
void pet_fruit_pointer(pet_fruit_t *s,const pet_p4_pointer_event_t *e) {
  if(!s||!e)return;
  if(e->x<0||e->x>639||e->y<0||e->y>479){s->down=s->menu_down=s->pause_down=false;s->combo=0;return;}
  /* A control owns its entire stroke, even after dragging off it. Slicing
     across a control never activates it. Physical pause clears this capture. */
  bool pause_hit=e->x>=528&&e->x<616&&e->y>=100&&e->y<148;
  if(s->phase==PET_FRUIT_PLAYING&&e->kind==PET_P4_POINTER_DOWN&&pause_hit){
    s->pause_down=s->menu_down=true;s->menu_x=e->x;s->menu_y=e->y;s->menu_at=e->ts_ms;
    s->down=false;s->combo=0;return;
  }
  if(s->pause_down){
    int dx=e->x-s->menu_x,dy=e->y-s->menu_y;
    if(dx*dx+dy*dy>144)s->menu_down=false;
    if(e->kind==PET_P4_POINTER_UP){
      bool tap=s->menu_down&&pause_hit&&e->ts_ms>=s->menu_at&&e->ts_ms-s->menu_at<700;
      s->pause_down=s->menu_down=false;
      if(tap&&s->phase==PET_FRUIT_PLAYING)pet_fruit_pause(s,e->ts_ms);
    }else if(e->kind==PET_P4_POINTER_CANCEL)s->pause_down=s->menu_down=false;
    return;
  }
  if(menu_pointer(s,e))return;
  if(e->kind==PET_P4_POINTER_UP||e->kind==PET_P4_POINTER_CANCEL||e->ts_ms<s->stroke_ms
      ||e->x<0||e->x>639||e->y<0||e->y>479){s->down=false;s->combo=0;return;}
  bool continued=s->down&&e->kind==PET_P4_POINTER_MOVE&&e->ts_ms-s->stroke_ms<=150;
  if(!continued||e->ts_ms-s->last_cut_ms>320)s->combo=0;
  int dx=e->x-s->px,dy=e->y-s->py;
  if(continued&&dx*dx+dy*dy>=9){
    /* Bombs win simultaneous intersections regardless of pool order. */
    for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<PET_FRUIT_MAX;i++){
      pet_fruit_item_t *f=&s->fruit[i];
      if(!f->active||f->cut||f->bomb!=(pass==0))continue;
      if(!hit(s->px-s->previous_x[i],s->py-s->previous_y[i],e->x-f->x,e->y-f->y,f->radius>0?f->radius:30))continue;
      s->feedback_x=f->x;s->feedback_y=f->y;s->feedback_ms=650;
      if(f->bomb){
        if(s->mode==PET_FRUIT_CLASSIC){finish(s,true);return;}
        f->active=false;s->feedback_points=s->score<30?s->score:30;s->score-=s->feedback_points;
        s->feedback_kind=3;s->flash_ms=250;s->sounds|=2;s->energy=s->fever_ms=s->combo=0;
        s->down=false;return;
      }
      f->cut=true;f->cut_age=0;s->cut_count++;s->combo++;s->sounds|=1;s->last_cut_ms=e->ts_ms;
      unsigned points=f->golden?50:10;
      if(s->combo>=3){points+=5*s->combo;s->combo_ms=650;s->feedback_combo=s->combo;}
      if(s->fever_ms)points*=2;
      s->score+=points;s->feedback_points=points;s->feedback_kind=f->golden?2:s->combo>=3?1:0;
      if(!s->fever_ms){s->energy+=f->golden?35:8+(s->combo>=3?8:0);if(s->energy>=100){s->energy=0;s->fever_ms=5000;}}
      if(s->score>s->records[s->mode])s->records[s->mode]=s->score;
      s->best=s->records[s->mode];
      if(s->combo>s->max_combo)s->max_combo=s->combo;
      for(unsigned n=0,at=0;n<8&&at<PET_FRUIT_PARTICLES;at++)if(!s->particles[at].life){
        s->particles[at]=(pet_fruit_particle_t){f->x,f->y,(int)(random_next(s)%301)-150,-80-(int)(random_next(s)%221),400+random_next(s)%250,f->kind};n++;
      }
    }
  }
  if(s->trail_count==PET_FRUIT_TRAIL){memmove(s->trail,s->trail+1,sizeof(s->trail[0])*(PET_FRUIT_TRAIL-1));s->trail_count--;}
  s->trail[s->trail_count++]=(pet_fruit_trail_t){e->x,e->y,e->ts_ms,!continued};
  s->px=e->x;s->py=e->y;s->stroke_ms=e->ts_ms;s->down=true;
  for(unsigned i=0;i<PET_FRUIT_MAX;i++){s->previous_x[i]=s->fruit[i].x;s->previous_y[i]=s->fruit[i].y;}
}
