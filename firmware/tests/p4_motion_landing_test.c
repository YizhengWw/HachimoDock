#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Exercise the production step, including camera movement and event dispatch. */
#include "../main/pet_p4_game.c"

static pet_p4_game_engine_t fixture(bool track, unsigned tick) {
  pet_p4_bounded_game_config_t c = {
    .width=16,.height=8,.tick_ms=tick,.entity_count=5,
    .motion=true,.motion_body=0,.platform_mask=30,.charge_ms=1200,
    .gravity=18,.lift=7,.launch_min=2,.launch_max=15,
    .track=track,.camera_x=4,.gap_min=2,.gap_max=5,
    .width_min=2,.width_max=4,.tone_mask=30,
  };
  c.entities[0]=(pet_p4_game_entity_t){.x=2,.y=4,.width=2,.height=2,.active=true};
  for(int i=1;i<5;i++) {
    c.entities[i]=(pet_p4_game_entity_t){.x=i==1?1:8,.y=6,.width=4,.height=1,.active=true};
    c.rules[c.rule_count++]=(pet_p4_game_rule_t){
      .trigger=PET_P4_GAME_TRIGGER_LANDED,.entity_index=0,.with_index=i,
      .op_count=1,.ops={{.kind=PET_P4_GAME_OP_SCORE_ADD,.value=1}},
    };
  }
  for(int i=0;i<5;i++){snprintf(c.entities[i].id,sizeof(c.entities[i].id),"e%d",i);c.entities[i].tone=1;}
  pet_p4_game_engine_t g={0};
  assert(pet_p4_game_configure_bounded(&g,&c,17));
  g.running=true;g.grounded=false;g.track_visited=0;
  /* Only target 2 is near the flight; keep other slots ahead, unrecycled. */
  for(int i=1;i<5;i++){g.bounded_entities[i].x_q8=(i==2?8:24+i*5)*256;g.bounded_entities[i].width=4;}
  return g;
}

int main(void) {
  unsigned checks=0;
  for(int track=0;track<2;track++)for(unsigned tick=16;tick<=40;tick+=4)
  for(int vx=-24;vx<=24;vx+=12)for(int offset=-256;offset<=4*256+256;offset+=8) {
    pet_p4_game_engine_t g=fixture(track,tick);
    pet_p4_game_entity_t *b=&g.bounded_entities[0],*p=&g.bounded_entities[2];
    const int dy=(10*256+18*256*(int)tick/1000)*(int)tick/1000;
    const int dx=vx*256*(int)tick/1000;
    const int contact_dx=(int)((int64_t)dx*16/dy);
    /* Force the swept foot centre to this exact platform-relative position. */
    b->x_q8=p->x_q8+offset-b->width*128-contact_dx;
    b->y_q8=p->y_q8-b->height*256-16;
    g.motion_vx_q8=vx*256;g.motion_vy_q8=10*256;
    bounded_motion_step(&g,20);
    /* The middle half of the sprite is its foot span. A point-only contact
     * does not count, nor do transparent outer corners hanging over a gap. */
    const bool expected=offset+128>0 && offset-128<4*256;
    assert(g.grounded==expected);
    assert(g.score==(expected?1:0));
    if(expected){
      assert(b->x_q8+b->width*128-p->x_q8==offset);
      assert(b->y_q8+b->height*256==p->y_q8);
      assert(g.motion_vx_q8==0&&g.motion_vy_q8==0);
      bounded_motion_step(&g,40);assert(g.score==1);
    }
    checks++;
  }
  for(int track=0;track<2;track++)for(int mode=0;mode<3;mode++) {
    pet_p4_game_engine_t g=fixture(track,20);
    pet_p4_game_entity_t *b=&g.bounded_entities[0],*p=&g.bounded_entities[2];
    b->x_q8=p->x_q8;
    b->y_q8=p->y_q8-b->height*256+(mode==0?1:mode==1?32:-32);
    g.motion_vy_q8=mode==0?10*256:-10*256;
    bounded_motion_step(&g,20);
    assert(!g.grounded&&g.score==0); /* below top, ascent through top, ascent above */
  }
  printf("PASS %u swept edge contacts; camera, 16–40ms, both horizontal directions, no side/ascent catch.\n",checks);
}
