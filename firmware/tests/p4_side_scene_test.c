#include "pet_p4_side_scene.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pet_side_config_t config;
static pet_side_scene_t scene;
static uint64_t clock_ms;
static void base(void){
  memset(&config,0,sizeof(config));config.width=40;config.height=13;config.tile_count=4;config.actor_count=1;config.lives=3;
  config.run_speed=96;config.acceleration=800;config.friction=1000;config.gravity=800;config.jump_speed=280;config.terminal_speed=480;
  config.tiles[1]=(pet_side_tile_t){.flags=PET_SIDE_SOLID};
  config.tiles[2]=(pet_side_tile_t){.flags=PET_SIDE_SOLID,.replacement=3,.reward=100};
  config.tiles[3]=(pet_side_tile_t){.flags=PET_SIDE_SOLID};
  for(int x=0;x<config.width;x++){config.cells[11*config.width+x]=1;config.cells[12*config.width+x]=1;}
  config.actors[0]=(pet_side_actor_def_t){.id="hero",.x=32,.y=160,.width=12,.height=16,.behavior=PET_SIDE_CONTROLLED};
}
static void start(void){assert(pet_side_init(&scene,&config));clock_ms=0;pet_side_restart(&scene,clock_ms);}
static void frames(unsigned count){for(unsigned i=0;i<count;i++){clock_ms+=20;pet_side_process(&scene,clock_ms);}}
static void test_run_and_wall(void){
  base();for(int y=8;y<11;y++)config.cells[y*config.width+6]=1;start();pet_side_axis(&scene,1);frames(100);
  assert(scene.actors[0].x+12*256==96*256);assert(scene.actors[0].grounded);
  pet_side_axis(&scene,-1);frames(15);assert(scene.actors[0].x<84*256);
  pet_side_axis(&scene,0);frames(30);int32_t x=scene.actors[0].x;frames(30);assert(scene.actors[0].x==x);
}
static void test_jump_and_release(void){
  base();start();frames(2);pet_side_jump(&scene,true,clock_ms);frames(10);int32_t high=scene.actors[0].y;
  assert(high<160*256);pet_side_jump(&scene,false,clock_ms);frames(50);assert(scene.actors[0].grounded&&scene.actors[0].y==160*256);
  start();frames(2);pet_side_jump(&scene,true,clock_ms);frames(2);pet_side_jump(&scene,false,clock_ms);frames(8);
  assert(scene.actors[0].y>high); // Short release genuinely reduces jump height.
  frames(50);assert(scene.actors[0].grounded);
}
static void test_reward_only_once(void){
  base();config.cells[8*config.width+2]=2;start();frames(2);pet_side_jump(&scene,true,clock_ms);frames(40);
  assert(scene.score==100&&scene.coins==1&&scene.cells[8*config.width+2]==3);
  pet_side_jump(&scene,false,clock_ms);pet_side_jump(&scene,true,clock_ms);frames(40);assert(scene.score==100&&scene.coins==1);
}
static void test_collect_and_goal(void){
  base();config.actor_count=3;
  config.actors[1]=(pet_side_actor_def_t){.id="coin",.x=48,.y=160,.width=8,.height=12,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_COLLECT,.value=50};
  config.actors[2]=(pet_side_actor_def_t){.id="exit",.x=144,.y=160,.width=16,.height=16,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_GOAL};
  start();pet_side_axis(&scene,1);frames(100);assert(scene.score==50&&scene.coins==1&&!scene.actors[1].active&&scene.phase==PET_SIDE_WON);
  uint32_t revision=scene.revision;frames(100);assert(scene.revision==revision);
}
static void test_damage_cannot_win_same_frame(void){
  base();config.actor_count=3;
  config.actors[1]=(pet_side_actor_def_t){.id="hazard",.x=32,.y=160,.width=12,.height=16,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_DAMAGE};
  config.actors[2]=(pet_side_actor_def_t){.id="exit",.x=32,.y=160,.width=12,.height=16,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_GOAL};
  start();frames(1);assert(scene.lives==2&&scene.hurt_ms&&scene.phase==PET_SIDE_RUNNING);
}
static void test_stomp_and_damage(void){
  base();config.actor_count=2;
  config.actors[1]=(pet_side_actor_def_t){.id="enemy",.x=80,.y=160,.width=16,.height=16,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_DAMAGE,.stompable=true,.value=200};
  start();scene.actors[0].x=82*256;scene.actors[0].y=143*256;scene.actors[0].vy=120*256;frames(1);
  assert(!scene.actors[1].active&&scene.score==200&&scene.actors[0].vy<0&&scene.lives==3);
  start();scene.actors[0].x=76*256;frames(1);assert(scene.lives==2&&scene.hurt_ms);
  frames(10);assert(scene.lives==2);frames(20);assert(scene.actors[0].x==32*256&&scene.invulnerable_ms>0);
}
static void test_checkpoint_and_fall(void){
  base();config.actor_count=2;
  config.actors[1]=(pet_side_actor_def_t){.id="checkpoint",.x=128,.y=160,.width=12,.height=16,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_CHECKPOINT};
  start();assert(!pet_side_restore_checkpoint(&scene,1));assert(pet_side_restore_checkpoint(&scene,2));assert(scene.actors[0].x==128*256);
  scene.actors[0].y=300*256;frames(1);assert(scene.lives==2);frames(26);assert(scene.actors[0].x==128*256);
  scene.actors[0].y=300*256;frames(1);assert(scene.lives==1);frames(26);scene.actors[0].y=300*256;frames(1);assert(scene.phase==PET_SIDE_LOST);
  pet_side_jump(&scene,true,clock_ms);assert(scene.phase==PET_SIDE_RUNNING&&scene.lives==3&&scene.score==0);
}
static void test_platform_carry(void){
  base();config.actor_count=2;config.actors[0].x=64;config.actors[0].y=112;
  config.actors[1]=(pet_side_actor_def_t){.id="lift",.x=64,.y=128,.width=32,.height=8,.behavior=PET_SIDE_PLATFORM,.solid=true,.speed=32,.range_min=64,.range_max=128};
  start();frames(2);assert(scene.carrier==1);int32_t x=scene.actors[0].x;frames(20);assert(scene.actors[0].x>x&&scene.actors[0].grounded);
  pet_side_jump(&scene,true,clock_ms);frames(2);assert(scene.carrier==-1&&!scene.actors[0].grounded);
}
static void test_one_way_and_hazard(void){
  base();config.tile_count=5;config.tiles[4].flags=PET_SIDE_ONE_WAY;config.jump_speed=320;
  for(int x=2;x<5;x++)config.cells[8*config.width+x]=4;
  start();frames(2);pet_side_jump(&scene,true,clock_ms);frames(35);
  assert(scene.actors[0].grounded&&scene.actors[0].y==112*256&&scene.lives==3);
  base();config.tile_count=5;config.tiles[4].flags=PET_SIDE_HAZARD;config.cells[10*config.width+2]=4;
  start();frames(1);assert(scene.lives==2&&scene.hurt_ms);
}
static void draw_checked(void*ctx,int x,int y,int w,int h,unsigned frame,bool flip){
  (void)x;(void)y;(void)flip;unsigned*count=ctx;(*count)++;assert(w>0&&h>0&&w<=96&&h<=96&&frame<256);
}
static void test_max_budget_and_long_input_trace(void){
  base();config.width=160;config.height=16;config.actor_count=24;memset(config.cells,0,sizeof(config.cells));
  for(int x=0;x<160;x++)config.cells[15*160+x]=1;config.actors[0].y=224;
  for(int i=1;i<24;i++)config.actors[i]=(pet_side_actor_def_t){.x=(uint16_t)(64+i*16),.y=224,.width=8,.height=8,.behavior=PET_SIDE_STATIC,.contact=PET_SIDE_COLLECT,.value=50};
  for(int i=1;i<24;i++)snprintf(config.actors[i].id,16,"object%d",i);
  start();uint32_t rng=42;
  for(int i=0;i<50000;i++){rng=rng*1664525u+1013904223u;pet_side_axis(&scene,(int)(rng%3)-1);if(i%17==0)pet_side_jump(&scene,true,clock_ms);if(i%17==8)pet_side_jump(&scene,false,clock_ms);frames(1);assert(scene.actors[0].x>=0&&scene.actors[0].x+12*256<=2560*256);}
  pet_side_art_t art={.columns=16,.rows=15,.scale=1,.w=640,.h=480,.skin_count=1};art.tiles[1]=1;
  art.skins[0]=(pet_side_skin_t){.idle={16},.walk={17},.air={18},.hurt={19},.counts={1,1,1,1},.fps=20,.width=32,.height=32};
  unsigned count=0;pet_side_draw(&scene,&art,draw_checked,&count);assert(count>0&&count<=704);
}
static void test_patrol_pause_and_budget(void){
  base();config.actor_count=2;config.actors[1]=(pet_side_actor_def_t){.id="patrol",.x=80,.y=160,.width=16,.height=16,.behavior=PET_SIDE_WALKER,.speed=30,.range_min=80,.range_max=160,.edge_turn=true};
  start();frames(200);assert(scene.actors[1].x>=80*256&&scene.actors[1].x<=160*256);
  pet_side_pause(&scene,true,clock_ms);pet_side_actor_t actor=scene.actors[1];frames(200);assert(!memcmp(&actor,&scene.actors[1],sizeof(actor)));
  pet_side_pause(&scene,false,clock_ms);pet_side_axis(&scene,1);int32_t x=scene.actors[0].x;clock_ms+=1000000;pet_side_process(&scene,clock_ms);assert(scene.actors[0].x-x<20*256);
  assert(pet_side_camera(&scene,320)>=0);assert(sizeof(scene)<16384);assert(sizeof(config)<8192);
}
static void test_invalid_configuration(void){
  base();assert(pet_side_config_valid(&config));config.width=161;assert(!pet_side_config_valid(&config));base();config.actor_count=2;config.actors[1]=config.actors[0];assert(!pet_side_config_valid(&config));base();config.cells[0]=15;assert(!pet_side_config_valid(&config));base();config.actors[0].y=176;assert(!pet_side_config_valid(&config));
  base();config.tiles[2].replacement=2;assert(!pet_side_config_valid(&config));
  base();start();assert(!pet_side_init(&scene,&scene.config));assert(scene.phase==PET_SIDE_RUNNING);
}
int main(void){
  test_run_and_wall();test_jump_and_release();test_reward_only_once();test_collect_and_goal();test_damage_cannot_win_same_frame();test_stomp_and_damage();test_checkpoint_and_fall();test_platform_carry();test_one_way_and_hazard();test_max_budget_and_long_input_trace();test_patrol_pause_and_budget();test_invalid_configuration();
  printf("PASS side scene: running, swept tiles, variable jump, one-shot rewards, collect/goal, stomp/damage, checkpoint/lives, moving platforms, patrol/pause/catch-up, bounds; scene=%zu config=%zu\n",sizeof(scene),sizeof(config));
}
