#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_navigation_core.h"
/* Local, fixed-memory navigation; never owns files, media or input hardware. */
static inline int pet_nav_page(const char pages[4][24],uint8_t count,const cJSON*v){
  if(!cJSON_IsString(v))return -1;
  for(int i=0;i<count;i++)if(!strcmp(pages[i],v->valuestring))return i;
  return -1;
}
static inline bool pet_nav_parse_rule(bool enabled,const char pages[4][24],uint8_t count,const cJSON*item,pet_nav_rule_t*out){
  *out=(pet_nav_rule_t){.from_page=-1,.target=-1};
  const cJSON *from=cJSON_GetObjectItemCaseSensitive(item,"from_page"),*p=cJSON_GetObjectItemCaseSensitive(item,"page");
  if(from){if(!enabled||(out->from_page=pet_nav_page(pages,count,from))<0)return false;}
  if(!p||cJSON_IsString(p))return true;
  if(!enabled||!cJSON_IsObject(p)||cJSON_GetArraySize(p)!=1)return false;
  const cJSON *op=p->child;
  if(!strcmp(op->string,"back")){out->op=PET_NAV_BACK;return cJSON_IsTrue(op);}
  if(!strcmp(op->string,"push"))out->op=PET_NAV_PUSH;
  else if(!strcmp(op->string,"replace"))out->op=PET_NAV_REPLACE;
  else if(!strcmp(op->string,"reset"))out->op=PET_NAV_RESET;
  else return false;
  out->target=pet_nav_page(pages,count,op);return out->target>=0;
}
