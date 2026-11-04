#include "pet_p4_side_scene_json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool keys(const cJSON*j,const char*allowed){
  if(!cJSON_IsObject(j))return false;
  const cJSON*v;cJSON_ArrayForEach(v,j){
    if(!v->string)return false;bool found=false;const char*p=allowed;
    while(*p){const char*end=strchr(p,'|');size_t n=end?(size_t)(end-p):strlen(p);if(strlen(v->string)==n&&!strncmp(v->string,p,n))found=true;if(!end)break;p=end+1;}
    if(!found)return false;
    for(const cJSON*prior=j->child;prior!=v;prior=prior->next)if(!strcmp(prior->string,v->string))return false;
  }return true;
}
static const cJSON*get(const cJSON*j,const char*k){return cJSON_GetObjectItemCaseSensitive(j,k);}
static bool number(const cJSON*j,const char*k,int low,int high,int fallback,int*out){
  const cJSON*v=get(j,k);if(!v&&fallback>=low&&fallback<=high){*out=fallback;return true;}
  if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||floor(v->valuedouble)!=v->valuedouble||v->valuedouble<low||v->valuedouble>high)return false;
  *out=(int)v->valuedouble;return true;
}
static bool boolean(const cJSON*j,const char*k,bool fallback,bool*out){const cJSON*v=get(j,k);if(!v){*out=fallback;return true;}if(!cJSON_IsBool(v))return false;*out=cJSON_IsTrue(v);return true;}
static bool text(const cJSON*j,const char*k,char*out,size_t n){
  const cJSON*v=get(j,k);if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>=n)return false;
  for(const unsigned char*p=(const unsigned char*)v->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;
  snprintf(out,n,"%s",v->valuestring);return true;
}
static int choice(const cJSON*j,const char*k,const char*const*names,int n){const cJSON*v=get(j,k);if(!cJSON_IsString(v))return -1;for(int i=0;i<n;i++)if(!strcmp(v->valuestring,names[i]))return i;return -1;}
static bool frames(const cJSON*j,const char*k,uint8_t*out,uint8_t*count,int limit){const cJSON*a=get(j,k);int n=cJSON_GetArraySize(a);if(!cJSON_IsArray(a)||n<1||n>8)return false;for(int i=0;i<n;i++){const cJSON*v=cJSON_GetArrayItem(a,i);if(!cJSON_IsNumber(v)||floor(v->valuedouble)!=v->valuedouble||v->valuedouble<0||v->valuedouble>=limit)return false;out[i]=(uint8_t)v->valueint;}*count=(uint8_t)n;return true;}
#define NUM(J,K,LO,HI,DEFAULT,DEST) do{int value;if(!number(J,K,LO,HI,DEFAULT,&value))goto bad;DEST=value;}while(0)
#define BOOL(J,K,DEFAULT,DEST) do{if(!boolean(J,K,DEFAULT,&DEST))goto bad;}while(0)
bool pet_side_parse(const cJSON*j,pet_side_declaration_t*out,char*error,size_t size){
  if(!out)return false;memset(out,0,sizeof(*out));pet_side_config_t*c=&out->config;pet_side_art_t*a=&out->art;
  if(!keys(j,"engine|width|height|rows|tiles|actors|physics|actions|art|active_state|result_state|win_state|score_var|coins_var|lives_var|checkpoint_var"))goto bad;
  const cJSON*engine=get(j,"engine");if(!cJSON_IsString(engine)||strcmp(engine->valuestring,"p4-side-scene-v1"))goto bad;
  NUM(j,"width",20,160,-1,c->width);NUM(j,"height",8,16,-1,c->height);
  const cJSON*physics=get(j,"physics");if(!keys(physics,"run_speed|acceleration|friction|gravity|jump_speed|terminal_speed|lives"))goto bad;
  NUM(physics,"run_speed",32,240,-1,c->run_speed);NUM(physics,"acceleration",100,2000,-1,c->acceleration);NUM(physics,"friction",100,2400,-1,c->friction);NUM(physics,"gravity",200,1800,-1,c->gravity);NUM(physics,"jump_speed",100,600,-1,c->jump_speed);NUM(physics,"terminal_speed",240,640,-1,c->terminal_speed);NUM(physics,"lives",1,9,-1,c->lives);
  const cJSON*tiles=get(j,"tiles");int count=cJSON_GetArraySize(tiles);if(!cJSON_IsArray(tiles)||count<1||count>16)goto bad;c->tile_count=count;
  for(int i=0;i<count;i++){const cJSON*t=cJSON_GetArrayItem(tiles,i);if(!keys(t,"flags|reward|replacement"))goto bad;NUM(t,"flags",0,15,-1,c->tiles[i].flags);NUM(t,"reward",0,10000,0,c->tiles[i].reward);NUM(t,"replacement",0,count-1,0,c->tiles[i].replacement);}
  const cJSON*rows=get(j,"rows");if(!cJSON_IsArray(rows)||cJSON_GetArraySize(rows)!=c->height)goto bad;
  for(int y=0;y<c->height;y++){const cJSON*r=cJSON_GetArrayItem(rows,y);if(!cJSON_IsString(r)||strlen(r->valuestring)!=c->width)goto bad;for(int x=0;x<c->width;x++){char ch=r->valuestring[x];int tile=ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:-1;if(tile<0||tile>=count)goto bad;c->cells[y*c->width+x]=tile;}}
  const cJSON*actors=get(j,"actors");count=cJSON_GetArraySize(actors);if(!cJSON_IsArray(actors)||count<1||count>24)goto bad;c->actor_count=count;
  const char*const behaviors[]={"controlled","walker","static","platform"},*const contacts[]={"none","damage","collect","goal","checkpoint"};
  for(int i=0;i<count;i++){
    const cJSON*o=cJSON_GetArrayItem(actors,i);pet_side_actor_def_t*d=&c->actors[i];
    if(!keys(o,"id|behavior|contact|x|y|width|height|skin|speed|range_min|range_max|value|vertical|solid|stompable|edge_turn")||!text(o,"id",d->id,sizeof(d->id)))goto bad;
    int behavior=choice(o,"behavior",behaviors,4),contact=choice(o,"contact",contacts,5);if(behavior<0||contact<0)goto bad;d->behavior=behavior;d->contact=contact;
    NUM(o,"x",0,c->width*16,-1,d->x);NUM(o,"y",0,c->height*16,-1,d->y);NUM(o,"width",1,32,-1,d->width);NUM(o,"height",1,32,-1,d->height);NUM(o,"skin",0,7,-1,d->skin);
    NUM(o,"speed",-160,160,0,d->speed);NUM(o,"range_min",0,2560,0,d->range_min);NUM(o,"range_max",0,2560,0,d->range_max);NUM(o,"value",0,10000,0,d->value);
    BOOL(o,"vertical",false,d->vertical);BOOL(o,"solid",false,d->solid);BOOL(o,"stompable",false,d->stompable);BOOL(o,"edge_turn",true,d->edge_turn);
  }
  if(!pet_side_config_valid(c))goto bad;
  if(!text(j,"active_state",out->active_state,24)||!text(j,"result_state",out->result_state,24)||!text(j,"win_state",out->win_state,24))goto bad;
  const char*vars[]={"score_var","coins_var","lives_var","checkpoint_var"};for(int i=0;i<4;i++)if(!text(j,vars[i],out->vars[i],32))goto bad;
  const cJSON*actions=get(j,"actions");const char*names[]={"left","right","jump","pause"};if(!keys(actions,"left|right|jump|pause"))goto bad;for(int i=0;i<4;i++){if(!text(actions,names[i],out->actions[i],48))goto bad;for(int p=0;p<i;p++)if(!strcmp(out->actions[p],out->actions[i]))goto bad;}
  const cJSON*art=get(j,"art");if(!keys(art,"image|columns|rows|scale|x|y|w|h|sky|tiles|skins")||!text(art,"image",a->image,16))goto bad;
  NUM(art,"columns",1,32,-1,a->columns);NUM(art,"rows",1,15,-1,a->rows);if(a->columns*a->rows>256)goto bad;
  NUM(art,"scale",1,3,-1,a->scale);NUM(art,"x",0,639,-1,a->x);NUM(art,"y",0,479,-1,a->y);NUM(art,"w",160,640-a->x,-1,a->w);NUM(art,"h",128,480-a->y,-1,a->h);NUM(art,"sky",0,65535,-1,a->sky);
  if(a->h<c->height*16*a->scale)goto bad;
  const cJSON*mapping=get(art,"tiles");if(!cJSON_IsArray(mapping)||cJSON_GetArraySize(mapping)!=c->tile_count)goto bad;
  for(int i=0;i<c->tile_count;i++){const cJSON*v=cJSON_GetArrayItem(mapping,i);if(!cJSON_IsNumber(v)||floor(v->valuedouble)!=v->valuedouble||v->valuedouble<0||v->valuedouble>=a->columns*a->rows)goto bad;a->tiles[i]=v->valueint;}
  const cJSON*skins=get(art,"skins");count=cJSON_GetArraySize(skins);if(!cJSON_IsArray(skins)||count<1||count>8)goto bad;a->skin_count=count;
  for(int i=0;i<count;i++){const cJSON*o=cJSON_GetArrayItem(skins,i);pet_side_skin_t*k=&a->skins[i];if(!keys(o,"idle|walk|air|hurt|fps|width|height"))goto bad;int max=a->columns*a->rows;
    if(!frames(o,"idle",k->idle,&k->counts[0],max)||!frames(o,"walk",k->walk,&k->counts[1],max)||!frames(o,"air",k->air,&k->counts[2],max)||!frames(o,"hurt",k->hurt,&k->counts[3],max))goto bad;
    NUM(o,"fps",1,20,-1,k->fps);NUM(o,"width",8,32,16,k->width);NUM(o,"height",8,32,16,k->height);
  }
  for(int i=0;i<c->actor_count;i++)if(c->actors[i].skin>=a->skin_count)goto bad;
  return true;
bad:if(error&&size)snprintf(error,size,"invalid bounded side scene declaration");return false;
}
