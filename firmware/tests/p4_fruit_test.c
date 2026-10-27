#include <assert.h>
#include <stdio.h>
#include "pet_p4_fruit_core.h"
static void point(pet_fruit_t*s,int kind,int x,int y,unsigned t){pet_p4_pointer_event_t e={kind,x,y,t};pet_fruit_pointer(s,&e);}
static void target(pet_fruit_t*s,unsigned i,float x,float y,bool bomb){s->fruit[i]=(pet_fruit_item_t){.active=true,.x=x,.y=y,.bomb=bomb};s->previous_x[i]=x;s->previous_y[i]=y;}
int main(void){
  pet_fruit_t s;pet_fruit_init(&s,60,3,55);assert(s.phase==PET_FRUIT_IDLE);
  pet_fruit_start(&s,0,42);assert(s.phase==PET_FRUIT_PLAYING);
  for(unsigned i=0;i<3;i++)target(&s,i,220+i*70,240,false);
  point(&s,0,160,240,1);point(&s,1,400,240,16);
  assert(s.score==45&&s.cut_count==3&&s.max_combo==3&&s.feedback_combo==3);
  pet_fruit_start_or_resume(&s,17,55);assert(s.score==45&&s.cut_count==3);
  point(&s,1,160,240,32);assert(s.score==45);point(&s,2,160,240,33);assert(!s.down);
  pet_fruit_pause(&s,40);pet_fruit_tick(&s,5000);assert(s.phase==PET_FRUIT_PAUSED&&s.elapsed_ms==0);
  pet_fruit_start_or_resume(&s,5000,99);pet_fruit_tick(&s,5016);assert(s.elapsed_ms==16&&s.score==45);
  pet_fruit_suspend(&s,5017);assert(s.phase==PET_FRUIT_PAUSED&&!s.down&&s.trail_count==0);
  pet_fruit_start(&s,0,42);target(&s,0,260,240,false);target(&s,1,320,240,true);
  point(&s,0,160,240,1);point(&s,1,400,240,16);assert(s.phase==PET_FRUIT_RESULT&&s.bomb_hit&&s.score==0);
  pet_fruit_start(&s,0,42);target(&s,0,260,240,false);
  point(&s,0,260,240,1);point(&s,1,260,240,16);assert(!s.score); /* holding never cuts */
  point(&s,3,260,240,17);point(&s,1,400,240,20);assert(!s.score); /* cancellation breaks sweep */
  point(&s,0,160,240,40);point(&s,1,400,240,300);assert(!s.score); /* stale stroke */
  point(&s,1,999,0,310);assert(!s.down);
  pet_fruit_start(&s,0,42);pet_fruit_tick(&s,500);assert(s.phase==PET_FRUIT_PAUSED);
  pet_fruit_start(&s,0,42);for(unsigned t=16;t<30000&&s.phase==PET_FRUIT_PLAYING;t+=16)pet_fruit_tick(&s,t);
  assert(s.phase==PET_FRUIT_RESULT&&s.misses==3&&!s.bomb_hit);
  pet_fruit_start(&s,0,42);s.elapsed_ms=59990;pet_fruit_tick(&s,16);assert(s.phase==PET_FRUIT_RESULT);
  pet_fruit_start(&s,0,42);assert(!s.score&&!s.misses&&!s.cut_count&&!s.bomb_hit);
  /* All three modes are selectable without changing a running round. */
  pet_fruit_init(&s,60,3,55);pet_fruit_pause(&s,0);assert(s.mode==PET_FRUIT_RUSH);
  point(&s,0,500,330,1);point(&s,2,500,330,2);assert(s.mode==PET_FRUIT_ZEN);
  point(&s,0,60,330,3);point(&s,1,130,330,4);point(&s,2,60,330,5);assert(s.mode==PET_FRUIT_ZEN);
  point(&s,0,300,420,6);point(&s,2,300,420,7);assert(s.phase==PET_FRUIT_PLAYING);
  pet_fruit_select_mode(&s,0);assert(s.mode==PET_FRUIT_ZEN);
  bool varied=false,golden=false;float first=0;
  for(unsigned t=23;t<60007&&s.phase==PET_FRUIT_PLAYING;t+=16){
    pet_fruit_tick(&s,t);
    for(unsigned i=0;i<PET_FRUIT_MAX;i++)if(s.fruit[i].active){
      assert(!s.fruit[i].bomb);assert(s.fruit[i].radius>=16&&s.fruit[i].radius<=38);
      if(first&&first!=s.fruit[i].radius)varied=true;
      first=s.fruit[i].radius;golden|=s.fruit[i].golden;
    }
  }
  pet_fruit_tick(&s,60008);assert(s.phase==PET_FRUIT_RESULT&&s.misses>3&&varied&&golden);
  pet_fruit_select_mode(&s,PET_FRUIT_RUSH);pet_fruit_start(&s,0,42);s.score=20;
  target(&s,0,260,240,true);point(&s,0,160,240,1);point(&s,1,400,240,16);
  assert(s.phase==PET_FRUIT_PLAYING&&s.score==0&&s.feedback_kind==3&&!s.fruit[0].active);
  /* Radius governs collision, not a constant shared by all fruit. */
  pet_fruit_start(&s,0,42);target(&s,0,260,240,false);s.fruit[0].radius=10;
  point(&s,0,160,270,1);point(&s,1,400,270,16);assert(!s.score);
  point(&s,2,400,270,17);s.fruit[0].radius=38;
  point(&s,0,160,270,18);point(&s,1,400,270,32);assert(s.score==10);
  /* Golden fruit powers a bounded fever; only subsequent cuts are doubled. */
  pet_fruit_start(&s,0,42);s.energy=70;target(&s,0,260,240,false);s.fruit[0].golden=true;
  point(&s,0,160,240,1);point(&s,1,400,240,16);assert(s.score==50&&s.fever_ms==5000&&s.energy==0);
  point(&s,2,400,240,17);target(&s,1,260,240,false);
  point(&s,0,160,240,18);point(&s,1,400,240,32);assert(s.score==70);
  assert(s.records[PET_FRUIT_RUSH]==70&&s.records[PET_FRUIT_CLASSIC]==0);
  pet_fruit_pause(&s,32);unsigned fever=s.fever_ms;
  pet_fruit_tick(&s,8000);assert(s.fever_ms==fever);
  pet_fruit_pause(&s,8000);pet_fruit_tick(&s,8016);assert(s.fever_ms==fever-16);
  pet_fruit_suspend(&s,8017);assert(s.phase==PET_FRUIT_PAUSED&&!s.down);
  pet_fruit_start(&s,0,42);target(&s,0,400,200,false);
  point(&s,0,560,124,10);point(&s,1,300,240,20);point(&s,2,560,124,30);
  assert(s.phase==PET_FRUIT_PLAYING&&!s.score&&!s.pause_down);
  point(&s,0,560,124,40);point(&s,3,560,124,50);point(&s,2,560,124,60);
  assert(s.phase==PET_FRUIT_PLAYING);
  point(&s,0,560,124,70);point(&s,2,560,124,800);
  assert(s.phase==PET_FRUIT_PLAYING); /* long hold is not a tap */
  point(&s,0,560,124,810);point(&s,2,560,124,820);
  assert(s.phase==PET_FRUIT_PAUSED&&!s.down&&!s.pause_down);
  point(&s,0,300,308,830);point(&s,2,300,308,840);
  assert(s.phase==PET_FRUIT_PLAYING&&!s.down);
  point(&s,0,500,180,850);point(&s,1,560,124,860);point(&s,2,560,124,870);
  assert(s.phase==PET_FRUIT_PLAYING); /* passing through is slicing, not pause */
  puts("fruit: combo, bomb, pause/resume, expiry, cancel, misses, restart passed");
}
