/* Strict form extension validation on the already parsed runtime. No allocation. */
#pragma once
#include "cJSON.h"
#include "pet_p4_app_form.h"
#include "pet_p4_json_source.h"
#include "pet_p4_variable_owners.h"
#include <math.h>
/* Reject unrepresentable C strings and malformed Unicode before cJSON can
 * lose lengths/retain invalid bytes; literal backslash-u stays ordinary text. */
static inline bool pet_form_source_valid(const char *json){
  return pet_json_source_bounded_text(json);
}
static inline const cJSON *pet_form_get(const cJSON *o,const char *k){return cJSON_GetObjectItemCaseSensitive(o,k);}
static inline const char *pet_form_string(const cJSON *o,const char *k){const cJSON*v=pet_form_get(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static inline bool pet_form_keys(const cJSON *o,const char *const *keys,int count){
  if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=count)return false;
  const cJSON*v;cJSON_ArrayForEach(v,o){int found=0;for(int i=0;i<count;i++)found+=!strcmp(v->string,keys[i]);if(found!=1)return false;
    for(const cJSON*p=o->child;p!=v;p=p->next)if(!strcmp(p->string,v->string))return false;}
  return true;
}
static inline bool pet_form_id(const char *s,size_t max){
  if(!s||!*s||strlen(s)>max)return false;
  for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.'))return false;
  return true;
}
static inline bool pet_form_integer(const cJSON*v,int32_t *out){
  if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<-1000000000||v->valuedouble>1000000000||v->valuedouble!=(int32_t)v->valuedouble)return false;
  *out=(int32_t)v->valuedouble;return true;
}
static inline int pet_form_var(const cJSON *vars,const char *name){
  if(!pet_form_id(name,31))return -1;
  int i=0;const cJSON*v;cJSON_ArrayForEach(v,vars){if(!strcmp(v->string,name))return !strcmp(pet_form_string(v,"type"),"int")&&i<PET_FORM_VARS?i:-1;i++;}return -1;
}
static inline bool pet_form_range(const cJSON *root,const char *name,int32_t lo,int32_t hi){
  const cJSON *range=pet_form_get(pet_form_get(root,"limits"),name);int32_t a,b;
  return !range||(cJSON_IsArray(range)&&cJSON_GetArraySize(range)==2&&pet_form_integer(cJSON_GetArrayItem(range,0),&a)&&pet_form_integer(cJSON_GetArrayItem(range,1),&b)&&a<=lo&&b>=hi);
}
static inline bool pet_form_contains(const cJSON *array,const char *s){const cJSON*v;cJSON_ArrayForEach(v,array)if(cJSON_IsString(v)&&!strcmp(v->valuestring,s))return true;return false;}
static inline bool pet_form_effects_safe(const cJSON *rule,const cJSON *vars,uint8_t forbidden){
  const char*const keys[]={"set","inc"};for(int k=0;k<2;k++){const cJSON*effects=pet_form_get(rule,keys[k]);if(effects&&!cJSON_IsObject(effects))return false;const cJSON*v;cJSON_ArrayForEach(v,effects){int i=pet_form_var(vars,v->string);if(i>=0&&(forbidden&(1u<<i)))return false;}}return true;
}
static inline bool pet_form_parse(const cJSON *root,const cJSON *buttons,pet_form_t *out){
  memset(out,0,sizeof(*out));
  const cJSON *form=pet_form_get(root,"form"),*surface=pet_form_get(root,"surface"),*nodes=pet_form_get(surface,"elements"),*v;
  if(!form){cJSON_ArrayForEach(v,nodes)if(pet_form_get(v,"field_bind"))return false;return true;}
  static const char*const fk[]={"engine","page","focus_var","active_var","fields","actions"};
  static const char*const blocked[]={"host","local","scene","game","data","media","instrument"};
  const cJSON *input=pet_form_get(root,"input"),*vars=pet_form_get(root,"vars"),*pages=pet_form_get(root,"pages"),*fields=pet_form_get(form,"fields");
  if(!pet_form_keys(form,fk,6)||strcmp(pet_form_string(form,"engine"),"p4-app-form-v1")||strcmp(pet_form_string(root,"engine"),"p4-bounded-runtime-v4")
    ||strcmp(pet_form_string(surface,"engine"),"p4-app-surface-v1")||strcmp(pet_form_string(surface,"graphics"),"p4-app-graphics-v1")
    ||strcmp(pet_form_string(surface,"bindings"),"p4-variable-bindings-v1")||strcmp(pet_form_string(input,"engine"),"p4-input-v1"))return false;
  pet_variable_owners_t writers;if(!pet_variable_owners_parse(root,&writers))return false;
  for(unsigned i=0;i<sizeof(blocked)/sizeof(*blocked);i++)if(pet_form_get(root,blocked[i])&&(i!=0||!writers.mixed))return false;
  if(!cJSON_IsObject(vars)||cJSON_GetArraySize(vars)>PET_FORM_VARS||!cJSON_IsArray(pages)||cJSON_GetArraySize(pages)<1||cJSON_GetArraySize(pages)>4||!cJSON_IsArray(fields)||cJSON_GetArraySize(fields)<1||cJSON_GetArraySize(fields)>PET_FORM_FIELDS)return false;
  int page=-1;for(int i=0;i<cJSON_GetArraySize(pages);i++)if(!strcmp(pet_form_string(cJSON_GetArrayItem(pages,i),"id"),pet_form_string(form,"page")))page=i;
  int focus=pet_form_var(vars,pet_form_string(form,"focus_var")),active=pet_form_var(vars,pet_form_string(form,"active_var"));
  if(page<0||focus<0||active<0||focus==active)return false;
  out->page=page;out->focus_var=focus;out->active_var=active;out->count=cJSON_GetArraySize(fields);
  int32_t initial[PET_FORM_VARS]={0};int at=0;cJSON_ArrayForEach(v,vars){if(at>=PET_FORM_VARS)return false;const cJSON*init=pet_form_get(v,"init");if(!strcmp(pet_form_string(v,"type"),"int")&&init&&!pet_form_integer(init,&initial[at]))return false;at++;}
  if(initial[focus]||initial[active]||!pet_form_range(root,pet_form_string(form,"focus_var"),0,out->count-1)||!pet_form_range(root,pet_form_string(form,"active_var"),0,1))return false;
  uint8_t owned=(1u<<focus)|(1u<<active),ephemeral=owned;
  for(int i=0;i<out->count;i++){
    const cJSON *q=cJSON_GetArrayItem(fields,i);pet_form_field_t*f=&out->fields[i];
    const char*id=pet_form_string(q,"id"),*name=pet_form_string(q,"var"),*draft=pet_form_string(q,"draft_var"),*kind=pet_form_string(q,"kind");
    int a=pet_form_var(vars,name),b=pet_form_var(vars,draft);
    if(!pet_form_id(id,15)||a<0||b<0||a==b||(owned&(1u<<a))||(owned&(1u<<b)))return false;
    for(int j=0;j<i;j++)if(!strcmp(out->fields[j].id,id))return false;
    strcpy(f->id,id);f->var=a;f->draft_var=b;owned|=(1u<<a)|(1u<<b);ephemeral|=1u<<b;
    if(!strcmp(kind,"choice")){
      static const char*const ck[]={"id","var","draft_var","kind","options"};const cJSON*options=pet_form_get(q,"options");
      if(!pet_form_keys(q,ck,5)||!cJSON_IsArray(options)||cJSON_GetArraySize(options)<2||cJSON_GetArraySize(options)>PET_FORM_OPTIONS)return false;
      f->option_count=cJSON_GetArraySize(options);
      for(int j=0;j<f->option_count;j++){
        static const char*const ok[]={"value","label"};const cJSON*o=cJSON_GetArrayItem(options,j);const char*label=pet_form_string(o,"label");
        if(!pet_form_keys(o,ok,2)||!pet_form_integer(pet_form_get(o,"value"),&f->values[j])||!*label||strlen(label)>63)return false;
        for(const unsigned char*p=(const unsigned char*)label;*p;p++)if(*p<32||*p==127)return false;
        for(int k=0;k<j;k++)if(f->values[k]==f->values[j])return false;
        strcpy(f->labels[j],label);if(!j||f->values[j]<f->min)f->min=f->values[j];if(!j||f->values[j]>f->max)f->max=f->values[j];
      }
    }else{
      static const char*const ik[]={"id","var","draft_var","kind","min","max","step"};
      if(strcmp(kind,"integer")||!pet_form_keys(q,ik,7)||!pet_form_integer(pet_form_get(q,"min"),&f->min)||!pet_form_integer(pet_form_get(q,"max"),&f->max)||!pet_form_integer(pet_form_get(q,"step"),&f->step)||f->min>=f->max||f->step<1||((int64_t)f->max-f->min)%f->step)return false;
    }
    if(!pet_form_value_valid(f,initial[a])||initial[a]!=initial[b]||!pet_form_range(root,name,f->min,f->max)||!pet_form_range(root,draft,f->min,f->max))return false;
  }
  static const char*const ops[]={"previous","next","decrease","increase","accept","cancel"};
  static const char*const reserved[]={"page_toggle","page_enter","page_back","page_main","page_app","component_center"};
  static const char*const physical[]={"button.sw1.short_press","button.sw2.short_press","button.encoder.short_press","button.encoder.long_press","knob.rotate_ccw","knob.rotate_cw","joystick.up","joystick.down"};
  const cJSON *actions=pet_form_get(form,"actions"),*rules=pet_form_get(root,"transitions");
  if(!pet_form_keys(actions,ops,6)||!cJSON_IsArray(rules)||!cJSON_IsArray(buttons)||cJSON_GetArraySize(buttons)>8)return false;
  cJSON_ArrayForEach(v,buttons){
    const char*event=pet_form_string(v,"event"),*action=pet_form_string(v,"action");
    bool known=!strcmp(event,"screen.region.tap")||!strcmp(event,"screen.region.long_press");
    for(unsigned i=0;i<sizeof(physical)/sizeof(*physical);i++)known|=!strcmp(event,physical[i]);
    if(!known)return false;
    for(int i=0;i<6;i++)if(!strcmp(action,reserved[i]))return false;
    for(const cJSON*p=buttons->child;p!=v;p=p->next)if(!strcmp(event,pet_form_string(p,"event"))||!strcmp(action,pet_form_string(p,"action")))return false;
  }
  for(int i=0;i<6;i++){
    const char*a=pet_form_string(actions,ops[i]);if(!pet_form_id(a,47)||!strncmp(a,"host.",5)||!strncmp(a,"input.",6))return false;
    for(int j=0;j<6;j++)if(!strcmp(a,reserved[j]))return false;
    for(int j=0;j<i;j++)if(!strcmp(a,out->actions[j]))return false;
    strcpy(out->actions[i],a);int count=0;bool bound=false;
    cJSON_ArrayForEach(v,rules)if(!strcmp(pet_form_string(v,"on"),a)){static const char*const rk[]={"from","on"};if(!pet_form_keys(v,rk,2)||strcmp(pet_form_string(v,"from"),"*"))return false;count++;}
    cJSON_ArrayForEach(v,buttons)if(!strcmp(pet_form_string(v,"action"),a))for(unsigned j=0;j<sizeof(physical)/sizeof(*physical);j++)if(!strcmp(pet_form_string(v,"event"),physical[j]))bound=true;
    if(count!=1||!bound||pet_form_contains(pet_form_get(input,"press"),a)||!strcmp(pet_form_string(input,"hold"),a)||pet_form_get(pet_form_get(input,"confirm"),a)||pet_form_get(pet_form_get(input,"cooldown"),a)||(i>=4&&pet_form_contains(pet_form_get(input,"repeat"),a)))return false;
  }
  cJSON_ArrayForEach(v,rules)if(!pet_form_effects_safe(v,vars,ephemeral)||!pet_form_effects_safe(pet_form_get(v,"else"),vars,ephemeral))return false;
  cJSON_ArrayForEach(v,pet_form_get(root,"tick"))if(!pet_form_effects_safe(v,vars,owned)||!pet_form_effects_safe(pet_form_get(v,"then"),vars,owned))return false;
  cJSON_ArrayForEach(v,pet_form_get(pet_form_get(root,"persist"),"vars")){int i=cJSON_IsString(v)?pet_form_var(vars,v->valuestring):-1;if(i>=0&&(ephemeral&(1u<<i)))return false;}
  out->enabled=true;
  cJSON_ArrayForEach(v,nodes)if(pet_form_get(v,"field_bind")){
    const char*t=pet_form_string(v,"type");if((strcmp(t,"text")&&strcmp(t,"button"))||pet_form_field_index(out,pet_form_string(v,"field_bind"))<0||pet_form_get(v,"text")||pet_form_get(v,"bind")||pet_form_get(v,"var_bind")||pet_form_get(v,"key_for"))return false;
  }
  return true;
}
