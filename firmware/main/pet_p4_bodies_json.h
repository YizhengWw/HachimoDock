/* Bodies schema resolver. Uses the caller's validated grid/entities; no allocation. */
#pragma once
#include "pet_p4_game.h"
#include "cJSON.h"
#include <string.h>
#include <math.h>
#ifndef PET_P4_BODIES
#define PET_P4_BODIES 0
#endif
static inline bool pet_body_number(const cJSON *v,int lo,int hi,int *out){
  if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<lo||v->valuedouble>hi||(double)(int)v->valuedouble!=v->valuedouble)return false;
  *out=(int)v->valuedouble;return true;
}
static inline bool pet_bodies_parse(const cJSON *scene,pet_p4_bounded_game_config_t *c){
  const cJSON *list=cJSON_GetObjectItemCaseSensitive(scene,"bodies"),*gravity=cJSON_GetObjectItemCaseSensitive(scene,"gravity");
  if(!list)return !gravity;
  if(!PET_P4_BODIES||c->motion||c->track||c->solid_tone_mask||c->tick_ms<20||c->tick_ms>200||!cJSON_IsArray(list)||cJSON_GetArraySize(list)<1||cJSON_GetArraySize(list)>8)return false;
  const cJSON *grid=cJSON_GetObjectItemCaseSensitive(scene,"grid");
  if(cJSON_HasObjectItem(grid,"rows")||cJSON_HasObjectItem(grid,"solid_tones"))return false;
  int n=0;if(gravity&&!pet_body_number(gravity,-64,64,&n))return false;c->body_gravity=n;
  bool seen[PET_P4_GAME_MAX_ENTITIES]={0};const cJSON *b;
  cJSON_ArrayForEach(b,list){
    if(!cJSON_IsObject(b))return false;
    const cJSON *field;cJSON_ArrayForEach(field,b)if(!field->string||(strcmp(field->string,"entity")&&strcmp(field->string,"mass")&&strcmp(field->string,"restitution")&&strcmp(field->string,"static")&&strcmp(field->string,"rolling")))return false;
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(b,"entity");if(!cJSON_IsString(id))return false;
    int index=-1;for(unsigned i=0;i<c->entity_count;i++)if(!strcmp(c->entities[i].id,id->valuestring))index=i;
    if(index<0||seen[index])return false;
    seen[index]=true;
    pet_p4_game_entity_t *e=&c->entities[index];e->body=true;e->mass=1;e->restitution=100;
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(b,"mass");if(v){if(!pet_body_number(v,1,64,&n))return false;e->mass=n;}
    v=cJSON_GetObjectItemCaseSensitive(b,"restitution");if(v){if(!pet_body_number(v,0,100,&n))return false;e->restitution=n;}
    v=cJSON_GetObjectItemCaseSensitive(b,"static");if(v&&!cJSON_IsBool(v))return false;e->body_static=cJSON_IsTrue(v);
    v=cJSON_GetObjectItemCaseSensitive(b,"rolling");if(v&&!cJSON_IsBool(v))return false;e->rolling=cJSON_IsTrue(v);
    if(e->rolling&&e->shape!=PET_P4_GAME_SHAPE_RECT&&e->shape!=PET_P4_GAME_SHAPE_CIRCLE&&e->shape!=PET_P4_GAME_SHAPE_BALL
      &&e->shape!=PET_P4_GAME_SHAPE_COIN&&e->shape!=PET_P4_GAME_SHAPE_CAPSULE&&e->shape!=PET_P4_GAME_SHAPE_TRIANGLE&&e->shape!=PET_P4_GAME_SHAPE_DIAMOND)return false;
    if(!e->collidable||e->bounds!=PET_P4_GAME_BOUNDS_CLAMP||(e->body_static&&(e->vx||e->vy)))return false;
  }
  for(unsigned i=0;i<c->entity_count;i++)if(!seen[i]&&(c->entities[i].vx||c->entities[i].vy))return false;
  c->bodies=true;return true;
}
