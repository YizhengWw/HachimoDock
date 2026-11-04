/* Trusted, bounded ABI for the same continuous world core used on the board. */
#include "pet_p4_side_scene.h"
#include <stddef.h>
void*memset(void*p,int v,size_t n){unsigned char*d=p;while(n--)*d++=v;return p;}
void*memcpy(void*p,const void*q,size_t n){unsigned char*d=p;const unsigned char*s=q;while(n--)*d++=*s++;return p;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static pet_side_config_t config;
static pet_side_scene_t scene;
static pet_side_art_t art;
static char text[64];
static int snapshot[11+PET_SIDE_ACTORS*7];
static int commands[1+704*6];
char*side_text(void){return text;}
void side_begin(int w,int h,int tiles,int actors,int lives,int speed,int acceleration,int friction,int gravity,int jump,int terminal){
  memset(&config,0,sizeof(config));memset(&art,0,sizeof(art));config.width=w;config.height=h;config.tile_count=tiles;config.actor_count=actors;config.lives=lives;config.run_speed=speed;config.acceleration=acceleration;config.friction=friction;config.gravity=gravity;config.jump_speed=jump;config.terminal_speed=terminal;
}
unsigned char*side_map(void){return config.cells;}
void side_tile(unsigned i,int flags,int reward,int replacement){if(i<16)config.tiles[i]=(pet_side_tile_t){.flags=flags,.reward=reward,.replacement=replacement};}
void side_actor(unsigned i,int x,int y,int w,int h,int skin,int behavior,int contact,int speed,int lo,int hi,int value,int flags){
  if(i>=24)return;pet_side_actor_def_t*a=&config.actors[i];*a=(pet_side_actor_def_t){.x=x,.y=y,.width=w,.height=h,.skin=skin,.behavior=behavior,.contact=contact,.speed=speed,.range_min=lo,.range_max=hi,.value=value,.vertical=!!(flags&1),.solid=!!(flags&2),.stompable=!!(flags&4),.edge_turn=!!(flags&8)};
  for(int n=0;n<15&&text[n];n++)a->id[n]=text[n];
}
void side_art(int columns,int rows,int scale,int x,int y,int w,int h,int sky,int skins){art.columns=columns;art.rows=rows;art.scale=scale;art.x=x;art.y=y;art.w=w;art.h=h;art.sky=sky;art.skin_count=skins;}
void side_art_tile(unsigned i,int frame){if(i<16)art.tiles[i]=frame;}
void side_skin(unsigned i,int fps,int w,int h){if(i<8){art.skins[i].fps=fps;art.skins[i].width=w;art.skins[i].height=h;}}
unsigned char*side_skin_frames(unsigned i,unsigned mode,unsigned count){if(i>=8||mode>=4||count>8)return 0;pet_side_skin_t*k=&art.skins[i];k->counts[mode]=count;return mode==0?k->idle:mode==1?k->walk:mode==2?k->air:k->hurt;}
int side_commit(void){return pet_side_init(&scene,&config);}
void side_axis(int direction){pet_side_axis(&scene,direction);}
void side_jump(int down,unsigned now){pet_side_jump(&scene,down,now);}
void side_pause(int pause,unsigned now){pet_side_pause(&scene,pause,now);}
void side_tick(unsigned now){pet_side_process(&scene,now);}
int side_checkpoint(unsigned value){return pet_side_restore_checkpoint(&scene,value);}
int*side_snapshot(void){
  snapshot[0]=scene.phase;snapshot[1]=scene.score;snapshot[2]=scene.coins;snapshot[3]=scene.lives;snapshot[4]=scene.checkpoint;snapshot[5]=scene.elapsed_ms;snapshot[6]=scene.revision;snapshot[7]=scene.hurt_ms;snapshot[8]=scene.invulnerable_ms;snapshot[9]=scene.config.actor_count;snapshot[10]=scene.player;
  for(int i=0;i<scene.config.actor_count;i++){pet_side_actor_t*a=&scene.actors[i];int*out=snapshot+11+i*7;out[0]=a->x;out[1]=a->y;out[2]=a->vx;out[3]=a->vy;out[4]=a->facing;out[5]=a->active;out[6]=a->grounded;}return snapshot;
}
static void draw(void*ctx,int x,int y,int w,int h,unsigned frame,bool flip){(void)ctx;int n=commands[0];if(n>=704)return;int*out=commands+1+n*6;out[0]=x;out[1]=y;out[2]=w;out[3]=h;out[4]=frame;out[5]=flip;commands[0]++;}
int*side_commands(void){commands[0]=0;pet_side_draw(&scene,&art,draw,0);return commands;}
unsigned char*side_cells(void){return scene.cells;}
