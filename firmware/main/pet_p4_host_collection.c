#include "pet_p4_host_collection.h"
#if PET_HOST_COLLECTION_ENABLED
#include "cJSON.h"
#include "pet_p4_collection.h"
#include <math.h>
#include <string.h>
static const cJSON *get(const cJSON *o,const char *key){return cJSON_GetObjectItemCaseSensitive(o,key);}
static bool keys(const cJSON *o,const char *const *names,int n){
  if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=n)return false;
  const cJSON *v;cJSON_ArrayForEach(v,o){bool known=false;for(int i=0;i<n;i++)if(!strcmp(v->string,names[i]))known=true;if(!known)return false;}
  return true;
}
static bool number(const cJSON *o,const char *key,int lo,int hi,int *out){
  const cJSON *v=get(o,key);if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<lo||v->valuedouble>hi||floor(v->valuedouble)!=v->valuedouble)return false;
  *out=(int)v->valuedouble;return true;
}
static bool name(const char *s,unsigned max){
  if(!s||!s[0]||strlen(s)>max)return false;
  for(const unsigned char *p=(const void*)s;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='_'||*p=='-'))return false;
  return true;
}
static bool engine(const cJSON *o){const cJSON *v=get(o,"engine");return cJSON_IsString(v)&&!strcmp(v->valuestring,PET_HOST_COLLECTION_ENGINE);}
static bool text(const cJSON *o,const char *key,char out[64],bool nonempty){
  const cJSON *v=get(o,key);if(!cJSON_IsString(v)||!pet_collection_text_valid(v->valuestring,63,nonempty))return false;
  strcpy(out,v->valuestring);return true;
}
bool pet_host_collection_parse_config(const cJSON *o,pet_host_collection_config_t *out){
  static const char*const allowed[]={"engine","source","x","y","w","row_height","gap","visible","page","actions","colors"};
  if(!out||!keys(o,allowed,11)||!engine(o))return false;
  memset(out,0,sizeof(*out));const cJSON *source=get(o,"source"),*actions=get(o,"actions"),*colors=get(o,"colors");int n;
  if(!cJSON_IsString(source)||!name(source->valuestring,16))return false;
  strcpy(out->source,source->valuestring);
  if(!number(o,"x",0,639,&n))return false;
  out->x=n;
  if(!number(o,"y",0,479,&n))return false;
  out->y=n;
  if(!number(o,"w",128,640-out->x,&n))return false;
  out->w=n;
  if(!number(o,"row_height",48,96,&n))return false;
  out->row_height=n;
  if(!number(o,"gap",0,16,&n))return false;
  out->gap=n;
  if(!number(o,"visible",1,8,&n))return false;
  out->visible=n;
  if(!number(o,"page",0,4,&n))return false;
  out->page=n;
  unsigned height=out->visible*out->row_height+(out->visible-1)*out->gap;
  if(height+out->y>480||!cJSON_IsArray(actions)||cJSON_GetArraySize(actions)!=out->visible||!cJSON_IsArray(colors)||cJSON_GetArraySize(colors)!=6)return false;
  for(unsigned i=0;i<out->visible;i++){
    const cJSON *a=cJSON_GetArrayItem(actions,i);
    if(!cJSON_IsString(a)||strncmp(a->valuestring,"host.",5)||!name(a->valuestring+5,42))return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(out->actions[j],a->valuestring))return false;
    strcpy(out->actions[i],a->valuestring);
  }
  for(unsigned i=0;i<6;i++){const cJSON *v=cJSON_GetArrayItem(colors,i);if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>65535||floor(v->valuedouble)!=v->valuedouble)return false;out->colors[i]=(uint16_t)v->valuedouble;}
  out->enabled=true;return true;
}
bool pet_host_collection_parse_snapshot(const cJSON *o,pet_host_collection_snapshot_t *out){
  static const char*const allowed[]={"engine","source","visible","count","offset","selectionKey","rows"};
  static const char*const rk[]={"present","key","label","detail","index","selected","truncated"};
  if(!out||!keys(o,allowed,7)||!engine(o))return false;
  memset(out,0,sizeof(*out));const cJSON *source=get(o,"source"),*rows=get(o,"rows"),*selection=get(o,"selectionKey");int n;
  if(!cJSON_IsString(source)||!name(source->valuestring,16))return false;
  strcpy(out->source,source->valuestring);
  if(!number(o,"visible",1,8,&n))return false;
  out->visible=n;
  if(!number(o,"count",0,128,&n))return false;
  out->count=n;
  if(!number(o,"offset",0,127,&n))return false;
  out->offset=n;
  unsigned last=(out->count?out->count-1:0)/out->visible*out->visible;
  if(out->offset>last||out->offset%out->visible||!cJSON_IsArray(rows)||cJSON_GetArraySize(rows)!=out->visible)return false;
  if(!cJSON_IsNull(selection)&&!text(o,"selectionKey",out->selection,true))return false;
  for(unsigned i=0;i<out->visible;i++){
    const cJSON *r=cJSON_GetArrayItem(rows,i);pet_host_collection_row_t *row=&out->rows[i];
    if(!keys(r,rk,7)||!cJSON_IsBool(get(r,"present"))||!cJSON_IsBool(get(r,"selected"))||!cJSON_IsBool(get(r,"truncated")))return false;
    row->present=cJSON_IsTrue(get(r,"present"));row->selected=cJSON_IsTrue(get(r,"selected"));row->truncated=cJSON_IsTrue(get(r,"truncated"));
    if(row->present!=(out->offset+i<out->count)||!text(r,"key",row->key,row->present)||!text(r,"label",row->label,false)||!text(r,"detail",row->detail,false)||!number(r,"index",-1,127,&n))return false;
    row->index=n;
    if(row->present){
      if(row->index!=(int)(out->offset+i)||row->selected!=(out->selection[0]&&!strcmp(row->key,out->selection)))return false;
      for(unsigned j=0;j<i;j++)if(!strcmp(row->key,out->rows[j].key))return false;
    }else if(row->key[0]||row->label[0]||row->detail[0]||row->index!=-1||row->selected||row->truncated)return false;
  }
  out->valid=true;return true;
}
bool pet_host_collection_matches(const pet_host_collection_config_t *c,const pet_host_collection_snapshot_t *s){
  return c&&s&&c->enabled&&s->valid&&c->visible==s->visible&&!strcmp(c->source,s->source);
}
#endif
