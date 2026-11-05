#include "pet_p4_side_scene.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define Q 256
#define CELL (PET_SIDE_CELL*Q)
static int32_t clamp(int32_t v,int32_t lo,int32_t hi){return v<lo?lo:v>hi?hi:v;}
static int32_t approach(int32_t v,int32_t target,int32_t delta){return v<target?clamp(v+delta,v,target):clamp(v-delta,target,v);}
static int tile_at(const pet_side_scene_t*s,int x,int y){
  if(x<0||x>=s->config.width)return -1;
  if(y<0||y>=s->config.height)return 0;
  return s->cells[y*s->config.width+x];
}
static int floor_cell(int32_t n){return n>=0?n/CELL:-((-n+CELL-1)/CELL);}
static bool overlaps(int32_t x,int32_t y,int w,int h,int32_t bx,int32_t by,int bw,int bh){
  return x<bx+bw*Q&&x+w*Q>bx&&y<by+bh*Q&&y+h*Q>by;
}
static bool name_valid(const char*n){
  size_t size=0;for(;size<16&&n[size];size++)if(!((n[size]>='a'&&n[size]<='z')||(n[size]>='A'&&n[size]<='Z')||(n[size]>='0'&&n[size]<='9')||n[size]=='_'||n[size]=='-'||n[size]=='.'))return false;
  return size>0&&size<16;
}
bool pet_side_config_valid(const pet_side_config_t*c){
  if(!c||c->width<20||c->width>PET_SIDE_WIDTH||c->height<8||c->height>PET_SIDE_HEIGHT||!c->tile_count||c->tile_count>PET_SIDE_TILES||!c->actor_count||c->actor_count>PET_SIDE_ACTORS||!c->lives||c->lives>9)return false;
  if(c->run_speed<32||c->run_speed>240||c->acceleration<100||c->acceleration>2000||c->friction<100||c->friction>2400||c->gravity<200||c->gravity>1800||c->jump_speed<100||c->jump_speed>600||c->terminal_speed<240||c->terminal_speed>640)return false;
  for(int i=0;i<c->width*c->height;i++)if(c->cells[i]>=c->tile_count)return false;
  if(c->tiles[0].flags||c->tiles[0].reward)return false;
  for(int i=0;i<c->tile_count;i++)if(c->tiles[i].flags>15||c->tiles[i].replacement>=c->tile_count||c->tiles[i].reward>10000||((c->tiles[i].flags&PET_SIDE_ONE_WAY)&&(c->tiles[i].flags&(PET_SIDE_SOLID|PET_SIDE_BREAK))))return false;
  for(int i=0;i<c->tile_count;i++)if(c->tiles[i].reward&&c->tiles[c->tiles[i].replacement].reward)return false;
  unsigned controlled=0;
  for(int i=0;i<c->actor_count;i++){
    const pet_side_actor_def_t*a=&c->actors[i];
    if(!name_valid(a->id)||!a->width||a->width>32||!a->height||a->height>32||a->x+a->width>c->width*16||a->y+a->height>c->height*16||a->skin>7||(unsigned)a->behavior>PET_SIDE_PLATFORM||(unsigned)a->contact>PET_SIDE_CHECKPOINT||a->value>10000)return false;
    for(int j=0;j<i;j++)if(!strcmp(a->id,c->actors[j].id))return false;
    controlled+=a->behavior==PET_SIDE_CONTROLLED;
    if(a->behavior==PET_SIDE_CONTROLLED&&(a->contact!=PET_SIDE_NONE||a->solid||a->speed))return false;
    if(a->behavior==PET_SIDE_STATIC&&(a->speed||a->solid))return false;
    if(a->behavior==PET_SIDE_WALKER||a->behavior==PET_SIDE_PLATFORM){
      if(!a->speed||a->speed<-160||a->speed>160||a->range_min>a->range_max)return false;
      unsigned extent=(a->vertical?c->height:c->width)*16,position=a->vertical?a->y:a->x;
      if((unsigned)(a->range_max+(a->vertical?a->height:a->width))>extent||position<a->range_min||position>a->range_max)return false;
      if(a->behavior==PET_SIDE_WALKER&&(a->vertical||a->solid))return false;
      if(a->behavior==PET_SIDE_PLATFORM&&(!a->solid||a->contact!=PET_SIDE_NONE))return false;
    }
    if(a->behavior==PET_SIDE_CONTROLLED){
      for(int y=a->y/16;y<=(a->y+a->height-1)/16;y++)for(int x=a->x/16;x<=(a->x+a->width-1)/16;x++)if(c->tiles[c->cells[y*c->width+x]].flags&PET_SIDE_SOLID)return false;
    }
  }
  return controlled==1;
}
static void reset_actor(pet_side_scene_t*s,int i){
  const pet_side_actor_def_t*d=&s->config.actors[i];pet_side_actor_t*a=&s->actors[i];
  memset(a,0,sizeof(*a));a->active=true;a->x=d->x*Q;a->y=d->y*Q;a->facing=d->speed<0?-1:1;
  if(d->behavior==PET_SIDE_WALKER)a->vx=d->speed*Q;
}
static void spawn(pet_side_scene_t*s){
  reset_actor(s,s->player);
  if(s->checkpoint){const pet_side_actor_def_t*c=&s->config.actors[s->checkpoint-1];s->actors[s->player].x=c->x*Q;s->actors[s->player].y=c->y*Q;}
  s->direction=0;s->carrier=-1;s->jump_down=false;s->jump_buffer_ms=0;s->coyote_ms=0;
}
void pet_side_restart(pet_side_scene_t*s,uint64_t now){
  if(!s)return;
  memcpy(s->cells,s->config.cells,sizeof(s->cells));
  for(int i=0;i<s->config.actor_count;i++)reset_actor(s,i);
  s->phase=PET_SIDE_RUNNING;s->score=0;s->coins=0;s->lives=s->config.lives;s->checkpoint=0;s->elapsed_ms=0;
  s->hurt_ms=s->invulnerable_ms=0;s->clock_ms=now;s->clock_set=true;spawn(s);s->revision++;
}
bool pet_side_init(pet_side_scene_t*s,const pet_side_config_t*c){
  if(!s||c==&s->config||!pet_side_config_valid(c))return false;
  memset(s,0,sizeof(*s));s->config=*c;
  for(int i=0;i<c->actor_count;i++)if(c->actors[i].behavior==PET_SIDE_CONTROLLED)s->player=(uint8_t)i;
  pet_side_restart(s,0);s->phase=PET_SIDE_READY;s->clock_set=false;return true;
}
void pet_side_axis(pet_side_scene_t*s,int dir){if(s&&s->phase==PET_SIDE_RUNNING&&!s->hurt_ms)s->direction=dir<0?-1:dir>0?1:0;}
void pet_side_jump(pet_side_scene_t*s,bool down,uint64_t now){
  if(!s)return;
  if(down&&(s->phase==PET_SIDE_READY||s->phase==PET_SIDE_LOST||s->phase==PET_SIDE_WON))pet_side_restart(s,now);
  if(s->phase!=PET_SIDE_RUNNING||s->hurt_ms)return;
  if(down&&!s->jump_down)s->jump_buffer_ms=120;
  if(!down&&s->jump_down&&s->actors[s->player].vy<-90*Q)s->actors[s->player].vy=-90*Q;
  s->jump_down=down;
}
void pet_side_pause(pet_side_scene_t*s,bool pause,uint64_t now){
  if(!s)return;
  if(pause&&s->phase==PET_SIDE_RUNNING)s->phase=PET_SIDE_PAUSED;
  else if(!pause&&s->phase==PET_SIDE_PAUSED)s->phase=PET_SIDE_RUNNING;
  s->direction=0;s->jump_buffer_ms=0;s->jump_down=false;s->clock_ms=now;s->clock_set=true;s->revision++;
}
static void award(pet_side_scene_t*s,int value){s->score=clamp(s->score+value,0,1000000000);}
static void damage(pet_side_scene_t*s,bool forced){
  if(s->hurt_ms||(!forced&&s->invulnerable_ms)||s->phase!=PET_SIDE_RUNNING)return;
  if(s->lives)s->lives--;
  if(!s->lives){s->phase=PET_SIDE_LOST;s->direction=0;return;}
  s->hurt_ms=500;s->direction=0;s->actors[s->player].vx=0;s->actors[s->player].vy=0;
}
static void hit_block(pet_side_scene_t*s,int x,int y){
  if(x<0||y<0||x>=s->config.width||y>=s->config.height)return;
  int pos=y*s->config.width+x,t=s->cells[pos];const pet_side_tile_t*d=&s->config.tiles[t];
  if(d->reward){award(s,d->reward);if(s->coins<65535)s->coins++;s->cells[pos]=d->replacement;}
  else if(d->flags&PET_SIDE_BREAK)s->cells[pos]=d->replacement;
}
/* Axis-separated 1-pixel sweeps bound work and prevent fast tile tunnelling. */
static bool move_axis(pet_side_scene_t*s,int i,int32_t delta,bool vertical){
  pet_side_actor_t*a=&s->actors[i];const pet_side_actor_def_t*d=&s->config.actors[i];bool blocked=false;
  while(delta){
    int32_t step=clamp(delta,-Q,Q),nx=a->x+(vertical?0:step),ny=a->y+(vertical?step:0);
    int x0=floor_cell(nx),x1=floor_cell(nx+d->width*Q-1),y0=floor_cell(ny),y1=floor_cell(ny+d->height*Q-1);
    for(int y=y0;y<=y1&&!blocked;y++)for(int x=x0;x<=x1;x++){
      int t=tile_at(s,x,y);unsigned flags=t<0?PET_SIDE_SOLID:s->config.tiles[t].flags;
      if(i==s->player&&(flags&PET_SIDE_HAZARD))damage(s,true);
      bool solid=flags&PET_SIDE_SOLID;
      if(vertical&&step>0&&(flags&PET_SIDE_ONE_WAY)&&a->y+d->height*Q<=y*CELL)solid=true;
      if(!solid)continue;
      if(vertical){
        if(step>0){ny=y*CELL-d->height*Q;a->grounded=true;}
        else{ny=(y+1)*CELL;if(i==s->player)hit_block(s,x,y);}
        a->vy=0;
      }else{nx=step>0?x*CELL-d->width*Q:(x+1)*CELL;a->vx=0;}
      blocked=true;break;
    }
    if(i==s->player&&!blocked){
      for(int p=0;p<s->config.actor_count;p++){
        const pet_side_actor_def_t*pd=&s->config.actors[p];pet_side_actor_t*pa=&s->actors[p];
        if(!pd->solid||!pa->active||p==i||!overlaps(nx,ny,d->width,d->height,pa->x,pa->y,pd->width,pd->height))continue;
        if(vertical){if(step>0&&a->y+d->height*Q<=pa->y){ny=pa->y-d->height*Q;a->grounded=true;s->carrier=(int8_t)p;a->vy=0;blocked=true;}else if(step<0&&a->y>=pa->y+pd->height*Q){ny=pa->y+pd->height*Q;a->vy=0;blocked=true;}}
        else{nx=step>0?pa->x-d->width*Q:pa->x+pd->width*Q;a->vx=0;blocked=true;}
        if(blocked)break;
      }
    }
    a->x=nx;a->y=ny;if(blocked)break;delta-=step;
  }
  return blocked;
}
static bool checkpoint_valid(const pet_side_scene_t*s,unsigned cp){
  if(!cp)return true;
  if(cp>s->config.actor_count||s->config.actors[cp-1].contact!=PET_SIDE_CHECKPOINT)return false;
  const pet_side_actor_def_t*c=&s->config.actors[cp-1],*p=&s->config.actors[s->player];
  if(c->x+p->width>s->config.width*16||c->y+p->height>s->config.height*16)return false;
  for(int y=c->y/16;y<=(c->y+p->height-1)/16;y++)for(int x=c->x/16;x<=(c->x+p->width-1)/16;x++)if(s->config.tiles[s->config.cells[y*s->config.width+x]].flags&(PET_SIDE_SOLID|PET_SIDE_HAZARD))return false;
  return true;
}
bool pet_side_restore_checkpoint(pet_side_scene_t*s,unsigned cp){
  if(!s||!checkpoint_valid(s,cp))return false;s->checkpoint=(uint8_t)cp;spawn(s);s->revision++;return true;
}
static void step(pet_side_scene_t*s){
  s->elapsed_ms+=PET_SIDE_STEP_MS;
  if(s->invulnerable_ms)s->invulnerable_ms=s->invulnerable_ms>PET_SIDE_STEP_MS?s->invulnerable_ms-PET_SIDE_STEP_MS:0;
  if(s->hurt_ms){s->hurt_ms=s->hurt_ms>PET_SIDE_STEP_MS?s->hurt_ms-PET_SIDE_STEP_MS:0;if(!s->hurt_ms){spawn(s);s->invulnerable_ms=1500;}s->revision++;return;}
  pet_side_actor_t*player=&s->actors[s->player];const pet_side_actor_def_t*body=&s->config.actors[s->player];int32_t old_y=player->y;
  for(int i=0;i<s->config.actor_count;i++){
    const pet_side_actor_def_t*d=&s->config.actors[i];pet_side_actor_t*a=&s->actors[i];if(!a->active||d->behavior!=PET_SIDE_PLATFORM)continue;
    int32_t before=d->vertical?a->y:a->x,delta=d->speed*a->facing*Q*PET_SIDE_STEP_MS/1000;
    /* facing is a path direction, independent of the sign of the speed. */
    if(d->speed<0)delta=-delta;
    int32_t after=before+delta;
    if(after<d->range_min*Q||after>d->range_max*Q){after=clamp(after,d->range_min*Q,d->range_max*Q);a->facing=-a->facing;}
    if(d->vertical)a->y=after;else a->x=after;
    if(s->carrier==i&&player->grounded){bool blocked=move_axis(s,s->player,after-before,d->vertical);if(blocked&&d->vertical&&after<before)damage(s,true);}
  }
  if(player->grounded)s->coyote_ms=80;else if(s->coyote_ms)s->coyote_ms=s->coyote_ms>PET_SIDE_STEP_MS?s->coyote_ms-PET_SIDE_STEP_MS:0;
  if(s->jump_buffer_ms&&s->coyote_ms){player->vy=-(int32_t)s->config.jump_speed*Q;player->grounded=false;s->coyote_ms=s->jump_buffer_ms=0;s->carrier=-1;}
  else if(s->jump_buffer_ms)s->jump_buffer_ms=s->jump_buffer_ms>PET_SIDE_STEP_MS?s->jump_buffer_ms-PET_SIDE_STEP_MS:0;
  int target=s->direction*s->config.run_speed*Q;
  player->vx=approach(player->vx,target,(s->direction?s->config.acceleration:s->config.friction)*Q*PET_SIDE_STEP_MS/1000);
  if(s->direction)player->facing=s->direction;
  move_axis(s,s->player,player->vx*PET_SIDE_STEP_MS/1000,false);
  player->vy=clamp(player->vy+s->config.gravity*Q*PET_SIDE_STEP_MS/1000,-640*Q,s->config.terminal_speed*Q);
  player->grounded=false;s->carrier=-1;move_axis(s,s->player,player->vy*PET_SIDE_STEP_MS/1000,true);
  if(player->y>s->config.height*CELL+32*Q)damage(s,true);
  if(s->hurt_ms||s->phase!=PET_SIDE_RUNNING){s->revision++;return;}
  for(int i=0;i<s->config.actor_count&&s->phase==PET_SIDE_RUNNING;i++){
    pet_side_actor_t*a=&s->actors[i];const pet_side_actor_def_t*d=&s->config.actors[i];if(!a->active||i==s->player)continue;
    if(d->behavior==PET_SIDE_WALKER){
      int32_t speed=(d->speed<0?-d->speed:d->speed)*Q*a->facing;
      int next_x=(a->x+(a->facing>0?d->width*Q:0)+a->facing*Q)/CELL,ground_y=(a->y+d->height*Q+Q)/CELL;
      int t=tile_at(s,next_x,ground_y);bool cliff=d->edge_turn&&a->grounded&&(t<0||!(s->config.tiles[t].flags&(PET_SIDE_SOLID|PET_SIDE_ONE_WAY)));
      a->vx=speed;
      if(cliff||move_axis(s,i,speed*PET_SIDE_STEP_MS/1000,false)||a->x<d->range_min*Q||a->x>d->range_max*Q){a->x=clamp(a->x,d->range_min*Q,d->range_max*Q);a->facing=-a->facing;}
      a->vy=clamp(a->vy+s->config.gravity*Q*PET_SIDE_STEP_MS/1000,-640*Q,s->config.terminal_speed*Q);a->grounded=false;move_axis(s,i,a->vy*PET_SIDE_STEP_MS/1000,true);
      if(a->y>s->config.height*CELL){a->active=false;continue;}
    }
    if(!overlaps(player->x,player->y,body->width,body->height,a->x,a->y,d->width,d->height))continue;
    if(d->contact==PET_SIDE_COLLECT){a->active=false;award(s,d->value);if(s->coins<65535)s->coins++;}
    else if(d->contact==PET_SIDE_GOAL)s->phase=PET_SIDE_WON;
    else if(d->contact==PET_SIDE_CHECKPOINT&&checkpoint_valid(s,(unsigned)i+1))s->checkpoint=(uint8_t)(i+1);
    else if(d->contact==PET_SIDE_DAMAGE){
      if(d->stompable&&player->vy>0&&old_y+body->height*Q<=a->y+2*Q){a->active=false;award(s,d->value);player->y=a->y-body->height*Q;player->vy=-(int32_t)s->config.jump_speed*Q*2/3;player->grounded=false;}
      else {damage(s,false);if(s->hurt_ms||s->phase!=PET_SIDE_RUNNING)break;}
    }
  }
  s->revision++;
}
bool pet_side_process(pet_side_scene_t*s,uint64_t now){
  if(!s)return false;
  if(!s->clock_set||now<s->clock_ms||s->phase!=PET_SIDE_RUNNING){s->clock_ms=now;s->clock_set=true;return false;}
  unsigned ticks=0;while(now-s->clock_ms>=PET_SIDE_STEP_MS&&ticks<8&&s->phase==PET_SIDE_RUNNING){s->clock_ms+=PET_SIDE_STEP_MS;step(s);ticks++;}
  if(now-s->clock_ms>=PET_SIDE_STEP_MS)s->clock_ms=now;
  return ticks>0;
}
int32_t pet_side_camera(const pet_side_scene_t*s,unsigned viewport){
  if(!s)return 0;int32_t max=s->config.width*16-(int32_t)viewport;if(max<0)max=0;
  return clamp(s->actors[s->player].x/Q-(int32_t)viewport*2/5,0,max);
}
void pet_side_draw(const pet_side_scene_t*s,const pet_side_art_t*a,pet_side_draw_fn draw,void*context){
  if(!s||!a||!draw||!a->scale||!a->columns||!a->rows||a->skin_count>8)return;
  int camera=pet_side_camera(s,a->w/a->scale),cell=16*a->scale;
  int first=camera/16,last=(camera+a->w/a->scale)/16+1;
  if(last>s->config.width)last=s->config.width;
  for(int y=0;y<s->config.height&&y*cell<a->h;y++)for(int x=first;x<last;x++){
    unsigned tile=s->cells[y*s->config.width+x];if(!tile||tile>=s->config.tile_count)continue;
    draw(context,a->x+(x*16-camera)*a->scale,a->y+y*cell,cell,cell,a->tiles[tile],false);
  }
  for(int i=0;i<s->config.actor_count;i++){
    const pet_side_actor_t*p=&s->actors[i];const pet_side_actor_def_t*d=&s->config.actors[i];
    if(!p->active||d->skin>=a->skin_count)continue;
    if(i==s->player&&s->invulnerable_ms&&(s->elapsed_ms/100)%2)continue;
    const pet_side_skin_t*k=&a->skins[d->skin];int mode=0;
    if(i==s->player&&s->hurt_ms)mode=3;
    else if(s->phase!=PET_SIDE_READY&&(d->behavior==PET_SIDE_CONTROLLED||d->behavior==PET_SIDE_WALKER)&&!p->grounded)mode=2;
    else if(s->phase!=PET_SIDE_READY&&(d->behavior==PET_SIDE_WALKER||(d->behavior==PET_SIDE_CONTROLLED&&(p->vx>Q||p->vx<-Q))))mode=1;
    const uint8_t*frames=mode==0?k->idle:mode==1?k->walk:mode==2?k->air:k->hurt;
    unsigned n=k->counts[mode];if(!n)continue;
    int x=a->x+((p->x/Q-camera)*2+d->width-k->width)*a->scale/2;
    int y=a->y+(p->y/Q+d->height-k->height)*a->scale;
    int w=k->width*a->scale,h=k->height*a->scale;
    if(x+w<=a->x||x>=a->x+a->w||y+h<=a->y||y>=a->y+a->h)continue;
    draw(context,x,y,w,h,frames[((uint64_t)s->elapsed_ms*k->fps/1000)%n],p->facing<0);
  }
}
