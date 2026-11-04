/* Trusted shared firmware game core. No imports, filesystem or network. */
#include "pet_p4_game.h"
#include <stddef.h>
void *memset(void *p,int v,size_t n){unsigned char*d=p;while(n--)*d++=v;return p;}
void *memcpy(void *p,const void*q,size_t n){unsigned char*d=p;const unsigned char*s=q;while(n--)*d++=*s++;return p;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static pet_p4_game_engine_t game;
void classic_hold_clock(unsigned now){game.last_tick_ms=now;}
static int result[262];
static pet_p4_bounded_game_config_t scene;
static char text[64];
static int entities[12*8+1];
char *scene_text(void){return text;}
void scene_begin(unsigned w,unsigned h,unsigned tick){memset(&scene,0,sizeof(scene));scene.width=w;scene.height=h;scene.tick_ms=tick;}
void scene_entity(unsigned i,int x,int y,int w,int h,int tone,int vx,int vy,int bounds,int shape,int active,int collidable){
  if(i>=12)return;pet_p4_game_entity_t *e=&scene.entities[i];memset(e,0,sizeof(*e));
  for(int j=0;j<15&&text[j];j++)e->id[j]=text[j];
  e->x=x;e->y=y;e->width=w;e->height=h;e->tone=tone;e->vx=vx;e->vy=vy;e->bounds=bounds;e->shape=shape;e->active=active;e->collidable=collidable;e->sprite_index=-1;
  if(scene.entity_count<=i)scene.entity_count=i+1;
}
void scene_bodies(int gravity){scene.bodies=true;scene.body_gravity=gravity;}
void scene_body(unsigned i,int mass,int restitution,int fixed,int rolling){
  if(i>=scene.entity_count)return;pet_p4_game_entity_t *e=&scene.entities[i];
  e->body=true;e->mass=mass;e->restitution=restitution;e->body_static=!!fixed;e->rolling=!!rolling;
}
static int physics[1+12*4];
int *scene_physics_snapshot(void){
  pet_p4_game_frame_t f;pet_p4_game_get_frame(&game,&f);physics[0]=f.entity_count;
  for(unsigned i=0;i<f.entity_count;i++){const pet_p4_game_entity_t *e=&f.entities[i];int *p=physics+1+i*4;p[0]=e->body_vx_q8;p[1]=e->body_vy_q8;p[2]=e->angle_q8;p[3]=e->body;}
  return physics;
}
void scene_motion(int body,int platforms,int gravity,int lift,int minimum,int maximum,int charge){scene.motion=true;scene.motion_body=body;scene.platform_mask=platforms;scene.gravity=gravity;scene.lift=lift;scene.launch_min=minimum;scene.launch_max=maximum;scene.charge_ms=charge;}
void scene_release(int cancel,unsigned now){pet_p4_game_release(&game,cancel,now);}
void scene_track(int camera,int gap_min,int gap_max,int width_min,int width_max,int tones){scene.track=true;scene.camera_x=camera;scene.gap_min=gap_min;scene.gap_max=gap_max;scene.width_min=width_min;scene.width_max=width_max;scene.tone_mask=tones;}
double scene_travel(void){return (double)game.camera_travel_q8/256;}
int scene_grounded(void){return game.grounded;}
int scene_charge(void){return game.frame.charge_percent;}
void scene_rule(unsigned i,int trigger,int entity,int with,int edge){
  if(i>=20)return;pet_p4_game_rule_t*r=&scene.rules[i];memset(r,0,sizeof(*r));r->trigger=trigger;r->entity_index=entity;r->with_index=with;r->edge=edge;
  for(int j=0;j<47&&text[j];j++)r->action[j]=text[j];
  if(scene.rule_count<=i)scene.rule_count=i+1;
}
/* p4-logic-v1: optional rule guard and runtime var mirror. */
void scene_rule_when(unsigned i,int var,int op,int value_var,int value){
  if(i>=20)return;pet_p4_game_rule_t*r=&scene.rules[i];r->has_when=true;r->when_var=var;r->when_op=op;r->when_value_var=value_var;r->when_value=value;
}
void scene_var(unsigned i,int value){if(i>=PET_P4_GAME_MAX_VARS)return;game.vars[i]=value;if(game.var_count<=i)game.var_count=i+1;}
int scene_var_get(unsigned i){return i<game.var_count?game.vars[i]:0;}
/* p4-sfx-v1: cue bits queued by rules since the last take. */
unsigned scene_sfx_take(void){unsigned m=game.sfx_pending;game.sfx_pending=0;return m;}
void scene_op(unsigned r,unsigned i,int kind,int entity,int source,int dx,int dy,int vx,int vy,int xmin,int xmax,int ymin,int ymax,int flags,int value,int tone,int axis){
  if(r>=20||i>=4)return;pet_p4_game_op_t*o=&scene.rules[r].ops[i];memset(o,0,sizeof(*o));o->kind=kind;o->entity_index=entity;o->source_index=source;o->dx=dx;o->dy=dy;o->vx=vx;o->vy=vy;
  o->x_min=xmin;o->x_max=xmax;o->y_min=ymin;o->y_max=ymax;o->has_x=flags&1;o->has_y=flags&2;o->random_x=flags&4;o->random_y=flags&8;o->value=value;o->tone=tone;o->axis_mask=axis;
  if(scene.rules[r].op_count<=i)scene.rules[r].op_count=i+1;
}
int scene_init(int autostart){
  if(!pet_p4_game_configure_bounded(&game,&scene,17))return 0;
  if(autostart){game.running=true;game.game_over=false;game.frame.running=true;game.frame.game_over=false;}return 1;
}
void scene_action(unsigned now){pet_p4_game_dispatch_action(&game,text,now);}
int scene_pointer(unsigned index,int x,int y,unsigned axes){return pet_p4_game_pointer(&game,index,x,y,axes);}
int *scene_snapshot(void){
  pet_p4_game_frame_t f;pet_p4_game_get_frame(&game,&f);entities[0]=f.entity_count;
  for(int i=0;i<f.entity_count;i++){const pet_p4_game_entity_t*e=&f.entities[i];int*a=entities+1+i*8;a[0]=(f.motion||f.bodies)?e->x_q8:e->x;a[1]=(f.motion||f.bodies)?e->y_q8:e->y;a[2]=e->width;a[3]=e->height;a[4]=e->tone;a[5]=e->shape;a[6]=e->active;a[7]=-1;for(int j=0;j<scene.entity_count;j++)if(!strcmp(e->id,scene.entities[j].id))a[7]=j;}return entities;
}
int classic_init(unsigned kind,unsigned tick,unsigned seed){return pet_p4_game_configure(&game,kind,tick,seed);}
int classic_vertical_speed(unsigned percent){return pet_p4_game_set_vertical_speed(&game,percent);}
/* gomoku: line reads of the last device reply; bounded by PET_P4_GAME_GOMOKU_AI_MAX_READS. */
int classic_gomoku_reads(void){return game.gomoku_ai_reads;}
void classic_action(unsigned command,unsigned now){pet_p4_game_command(&game,command,now);}
void classic_tick(unsigned now){pet_p4_game_process(&game,now);}
int *classic_snapshot(void){
  pet_p4_game_frame_t f;pet_p4_game_get_frame(&game,&f);
  result[0]=f.width;result[1]=f.height;result[2]=f.score;result[3]=f.running;result[4]=f.game_over;result[5]=f.kind;
  for(int i=0;i<256;i++)result[i+6]=f.cells[i];
  return result;
}
