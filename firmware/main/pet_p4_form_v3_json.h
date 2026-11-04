/* Strict, capability-gated p4-app-form-v3 declaration parser. Never repairs. */
#pragma once
#include "pet_p4_app_form_json.h"
#include "pet_p4_form_v2_json.h"
#include "pet_p4_form_v3.h"
#include "pet_p4_text_flow.h"
static inline int pet_form3_var(const cJSON *vars,const char *name,const char *type){
  if(!pet_form_id(name,31))return -1;int i=0;const cJSON*v;
  cJSON_ArrayForEach(v,vars){if(!strcmp(v->string,name))return i<PET_FORM3_VARS&&!strcmp(pet_form_string(v,"type"),type)?i:-1;i++;}return -1;
}
static inline bool pet_form3_safe(const cJSON *rule,const cJSON *vars,uint32_t mask){
  const char*const ops[]={"set","inc"};const cJSON*v;
  for(unsigned k=0;k<2;k++){const cJSON*effects=pet_form_get(rule,ops[k]);if(effects&&!cJSON_IsObject(effects))return false;
    cJSON_ArrayForEach(v,effects){int i=pet_form3_var(vars,v->string,"int");if(i<0)i=pet_form3_var(vars,v->string,"string");if(i>=0&&(mask&(1U<<i)))return false;}}
  return true;
}
/* buttons: the 8 physical bindings of this package (one per logical action). */
static inline bool pet_form3_parse(const cJSON *root,const cJSON *buttons,pet_form3_t *out){
  if(!out)return false;
  pet_form3_t form={0};
  const cJSON*f=pet_form_get(root,"form"),*s=pet_form_get(root,"surface"),*input=pet_form_get(root,"input"),*vars=pet_form_get(root,"vars"),*pages=pet_form_get(root,"pages"),*fields=pet_form_get(f,"fields"),*v;
  const char*const fk[]={"engine","page","focus_var","active_var","fields","actions","keyboard"};
  if(!pet_form_keys(f,fk,7)||strcmp(pet_form_string(f,"engine"),PET_FORM3_ENGINE)||strcmp(pet_form_string(root,"engine"),"p4-bounded-runtime-v4")
      ||strcmp(pet_form_string(s,"engine"),"p4-app-surface-v1")||strcmp(pet_form_string(s,"graphics"),"p4-app-graphics-v1")
      ||strcmp(pet_form_string(s,"bindings"),"p4-variable-bindings-v1")||strcmp(pet_form_string(input,"engine"),"p4-input-v1"))return false;
  const char*const blocked[]={"host","local","scene","game","data","media","instrument"};
  pet_variable_owners_t writers={0};if(pet_form_get(root,"ownership")&&!pet_variable_owners_parse(root,&writers))return false;
  for(unsigned i=0;i<sizeof(blocked)/sizeof(*blocked);i++)if(pet_form_get(root,blocked[i])&&(i!=0||!writers.mixed))return false;
  int var_count=cJSON_IsObject(vars)?cJSON_GetArraySize(vars):-1;
  const char *quota=pet_form_string(root,"quota");
  int max_vars=!strcmp(quota,"p4-quota-v3")?32:!strcmp(quota,"p4-quota-v2")?16:8;unsigned max_string=max_vars==32?127:63;
  if(var_count<1||var_count>max_vars)return false;
  if(!cJSON_IsArray(pages)||cJSON_GetArraySize(pages)<1||cJSON_GetArraySize(pages)>12||!cJSON_IsArray(fields)||cJSON_GetArraySize(fields)<1||cJSON_GetArraySize(fields)>PET_FORM3_FIELDS)return false;
  int page=-1;for(int i=0;i<cJSON_GetArraySize(pages);i++)if(!strcmp(pet_form_string(cJSON_GetArrayItem(pages,i),"id"),pet_form_string(f,"page")))page=i;
  int focus=pet_form3_var(vars,pet_form_string(f,"focus_var"),"int"),active=pet_form3_var(vars,pet_form_string(f,"active_var"),"int");
  if(page<0||focus<0||active<0||focus==active)return false;
  form.enabled=true;form.page=page;form.count=cJSON_GetArraySize(fields);form.focus_var=focus;form.active_var=active;
  pet_form3_value_t initial[PET_FORM3_VARS]={0};int index=0;
  cJSON_ArrayForEach(v,vars){const cJSON*value=pet_form_get(v,"init");const char*type=pet_form_string(v,"type");
    if(!strcmp(type,"int")){if(value&&!pet_form_integer(value,&initial[index].number))return false;}
    else if(!strcmp(type,"string")){const char*str=value&&cJSON_IsString(value)?value->valuestring:"";if((value&&!cJSON_IsString(value))||(!pet_text_flow_valid(str,max_string)||strchr(str,'\n')))return false;if(strlen(str)<=63)strcpy(initial[index].text,str);}
    else return false;index++;}
  if(initial[focus].number||initial[active].number||!pet_form_range(root,pet_form_string(f,"focus_var"),0,form.count-1)||!pet_form_range(root,pet_form_string(f,"active_var"),0,1))return false;
  uint32_t owned=(1U<<focus)|(1U<<active),ephemeral=owned;
  for(int i=0;i<form.count;i++){
    const cJSON*q=cJSON_GetArrayItem(fields,i);pet_form3_field_t*p=&form.fields[i];
    const char*const keys[]={"id","var","draft_var","kind","max_bytes","keyboard"};int32_t max;
    const char*id=pet_form_string(q,"id"),*name=pet_form_string(q,"var"),*draft=pet_form_string(q,"draft_var");
    int a=pet_form3_var(vars,name,"string"),b=pet_form3_var(vars,draft,"string"),mode=pet_form3_mode(pet_form_string(q,"keyboard"));
    if(!pet_form_keys(q,keys,6)||strcmp(pet_form_string(q,"kind"),"text")||mode<0||!pet_form_id(id,15)||a<0||b<0||a==b||(owned&((1U<<a)|(1U<<b))))return false;
    for(int j=0;j<i;j++)if(!strcmp(form.fields[j].id,id))return false;
    if(!pet_form_integer(pet_form_get(q,"max_bytes"),&max)||max<1||max>63||!pet_form_utf8(initial[a].text,max,NULL,NULL,NULL)||strcmp(initial[a].text,initial[b].text)
        ||pet_form_get(pet_form_get(root,"limits"),name)||pet_form_get(pet_form_get(root,"limits"),draft))return false;
    strcpy(p->id,id);p->var=a;p->draft_var=b;p->mode=mode;p->max_bytes=max;owned|=(1U<<a)|(1U<<b);ephemeral|=1U<<b;
  }
  const cJSON*k=pet_form_get(f,"keyboard");
  const char*const kk[]={"key_var","cursor_var","buffer_var","label_var","candidates_var"};
  if(!pet_form_keys(k,kk,5))return false;
  int roles[5];const char*types[5]={"int","int","string","string","string"};
  for(unsigned i=0;i<5;i++){
    const char*n=pet_form_string(k,kk[i]);roles[i]=pet_form3_var(vars,n,types[i]);
    if(roles[i]<0||(owned&(1U<<roles[i])))return false;owned|=1U<<roles[i];ephemeral|=1U<<roles[i];
    if(i<2){if(initial[roles[i]].number||!pet_form_range(root,n,0,i==0?PET_FORM3_NUMERIC_KEYS-1:PET_FORM3_CANDIDATES_MAX-1))return false;}
    else if(initial[roles[i]].text[0]||pet_form_get(pet_form_get(root,"limits"),n))return false;
  }
  form.key_var=roles[0];form.cursor_var=roles[1];form.buffer_var=roles[2];form.label_var=roles[3];form.candidates_var=roles[4];
  const char*const ops[]={"previous","next","decrease","increase","accept","cancel","append","erase"};
  const char*const reserved[]={"page_toggle","page_enter","page_back","page_main","page_app","component_center","tab.next","tab.prev","data.refresh"};
  const char*const physical[]={"button.sw1.short_press","button.sw2.short_press","button.encoder.short_press","button.encoder.long_press","knob.rotate_ccw","knob.rotate_cw","joystick.up","joystick.down"};
  const cJSON*actions=pet_form_get(f,"actions"),*rules=pet_form_get(root,"transitions");
  if(!pet_form_keys(actions,ops,8)||!cJSON_IsArray(rules)||!cJSON_IsArray(buttons)||cJSON_GetArraySize(buttons)!=8)return false;
  cJSON_ArrayForEach(v,buttons){bool known=false;for(unsigned i=0;i<8;i++)known|=!strcmp(pet_form_string(v,"event"),physical[i]);if(!known)return false;
    for(const cJSON*p=buttons->child;p!=v;p=p->next)if(!strcmp(pet_form_string(v,"event"),pet_form_string(p,"event"))||!strcmp(pet_form_string(v,"action"),pet_form_string(p,"action")))return false;}
  for(unsigned i=0;i<8;i++){
    const char*a=pet_form_string(actions,ops[i]);if(!pet_form_id(a,47)||!strncmp(a,"host.",5)||!strncmp(a,"input.",6))return false;
    for(unsigned j=0;j<sizeof(reserved)/sizeof(*reserved);j++)if(!strcmp(a,reserved[j]))return false;
    strcpy(form.actions[i],a);
    int count=0,bound=0;cJSON_ArrayForEach(v,rules)if(!strcmp(pet_form_string(v,"on"),a)){const char*const keys[]={"from","on"};if(!pet_form_keys(v,keys,2)||strcmp(pet_form_string(v,"from"),"*"))return false;count++;}
    cJSON_ArrayForEach(v,buttons)bound+=!strcmp(pet_form_string(v,"action"),a);
    if(count!=1||bound!=1||pet_form_contains(pet_form_get(input,"press"),a)||!strcmp(pet_form_string(input,"hold"),a)||pet_form_get(pet_form_get(input,"confirm"),a)||pet_form_get(pet_form_get(input,"cooldown"),a)||(i>=4&&pet_form_contains(pet_form_get(input,"repeat"),a)))return false;
  }
  cJSON_ArrayForEach(v,rules)if(!pet_form3_safe(v,vars,owned)||!pet_form3_safe(pet_form_get(v,"else"),vars,owned))return false;
  cJSON_ArrayForEach(v,pet_form_get(root,"tick"))if(!pet_form3_safe(v,vars,owned)||!pet_form3_safe(pet_form_get(v,"then"),vars,owned))return false;
  cJSON_ArrayForEach(v,pet_form_get(pet_form_get(root,"persist"),"vars")){int i=cJSON_IsString(v)?pet_form3_var(vars,v->valuestring,"int"):-1;if(i<0&&cJSON_IsString(v))i=pet_form3_var(vars,v->valuestring,"string");if(i>=0&&(ephemeral&(1U<<i)))return false;}
  cJSON_ArrayForEach(v,pet_form_get(s,"elements"))if(pet_form_get(v,"field_bind")){const char*type=pet_form_string(v,"type");if((strcmp(type,"text")&&strcmp(type,"button"))||pet_form3_field_index(&form,pet_form_string(v,"field_bind"))<0||pet_form_get(v,"text")||pet_form_get(v,"bind")||pet_form_get(v,"var_bind")||pet_form_get(v,"key_for"))return false;}
  index=0;cJSON_ArrayForEach(v,vars){if((owned&(1U<<index))&&!strcmp(pet_form_string(v,"type"),"string")){const cJSON*value=pet_form_get(v,"init");if(value&&!pet_form_utf8(value->valuestring,63,NULL,NULL,NULL))return false;}index++;}
  if(!pet_form3_valid(&form))return false;
  *out=form;return true;
}
