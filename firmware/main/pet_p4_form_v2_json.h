/* Strict, capability-gated mixed form declaration. Never repairs a package. */
#pragma once
#include "pet_p4_app_form_json.h"
#include "pet_p4_form_v2.h"
static inline int pet_form2_var(const cJSON *vars,const char *name,const char *type){
  if(!pet_form_id(name,31))return -1;int i=0;const cJSON*v;
  cJSON_ArrayForEach(v,vars){if(!strcmp(v->string,name))return i<PET_FORM_VARS&&!strcmp(pet_form_string(v,"type"),type)?i:-1;i++;}return -1;
}
static inline bool pet_form2_safe(const cJSON *rule,const cJSON *vars,uint8_t mask){
  const char*const ops[]={"set","inc"};const cJSON*v;
  for(unsigned k=0;k<2;k++){const cJSON*effects=pet_form_get(rule,ops[k]);if(effects&&!cJSON_IsObject(effects))return false;
    cJSON_ArrayForEach(v,effects){int i=pet_form2_var(vars,v->string,"int");if(i<0)i=pet_form2_var(vars,v->string,"string");if(i>=0&&(mask&(1U<<i)))return false;}}
  return true;
}
static inline bool pet_form2_parse(const cJSON *root,const cJSON *buttons,pet_form_t *out,pet_form_text_config_t *text_out){
  if(!out||!text_out)return false;
  pet_form_t form={0};pet_form_text_config_t text={0};
  const cJSON*f=pet_form_get(root,"form"),*s=pet_form_get(root,"surface"),*input=pet_form_get(root,"input"),*vars=pet_form_get(root,"vars"),*pages=pet_form_get(root,"pages"),*fields=pet_form_get(f,"fields"),*v;
  const char*const fk[]={"engine","page","focus_var","active_var","fields","actions","keyboard"};
  if(!pet_form_keys(f,fk,7)||strcmp(pet_form_string(f,"engine"),"p4-app-form-v2")||strcmp(pet_form_string(root,"engine"),"p4-bounded-runtime-v4")
      ||strcmp(pet_form_string(s,"engine"),"p4-app-surface-v1")||strcmp(pet_form_string(s,"graphics"),"p4-app-graphics-v1")
      ||strcmp(pet_form_string(s,"bindings"),"p4-variable-bindings-v1")||strcmp(pet_form_string(input,"engine"),"p4-input-v1"))return false;
  const char*const blocked[]={"host","local","scene","game","data","media","instrument"};
  pet_variable_owners_t writers;if(!pet_variable_owners_parse(root,&writers))return false;
  for(unsigned i=0;i<sizeof(blocked)/sizeof(*blocked);i++)if(pet_form_get(root,blocked[i])&&(i!=0||!writers.mixed))return false;
  if(!cJSON_IsObject(vars)||cJSON_GetArraySize(vars)>PET_FORM_VARS||!cJSON_IsArray(pages)||cJSON_GetArraySize(pages)<1||cJSON_GetArraySize(pages)>4||!cJSON_IsArray(fields)||cJSON_GetArraySize(fields)<1||cJSON_GetArraySize(fields)>3)return false;
  int page=-1;for(int i=0;i<cJSON_GetArraySize(pages);i++)if(!strcmp(pet_form_string(cJSON_GetArrayItem(pages,i),"id"),pet_form_string(f,"page")))page=i;
  int focus=pet_form_var(vars,pet_form_string(f,"focus_var")),active=pet_form_var(vars,pet_form_string(f,"active_var"));
  if(page<0||focus<0||active<0||focus==active)return false;
  form.enabled=true;form.page=page;form.count=cJSON_GetArraySize(fields);form.focus_var=focus;form.active_var=active;text.enabled=true;
  pet_form_value_t initial[8]={0};int index=0;
  cJSON_ArrayForEach(v,vars){const cJSON*value=pet_form_get(v,"init");const char*type=pet_form_string(v,"type");
    if(!strcmp(type,"int")){if(value&&!pet_form_integer(value,&initial[index].number))return false;}
    else if(!strcmp(type,"string")){const char*str=value&&cJSON_IsString(value)?value->valuestring:"";if((value&&!cJSON_IsString(value))||!pet_form_utf8(str,63,NULL,NULL,NULL))return false;strcpy(initial[index].text,str);}
    else return false;index++;}
  if(initial[focus].number||initial[active].number||!pet_form_range(root,pet_form_string(f,"focus_var"),0,form.count-1)||!pet_form_range(root,pet_form_string(f,"active_var"),0,1))return false;
  uint8_t owned=(1U<<focus)|(1U<<active),ephemeral=owned;
  for(int i=0;i<form.count;i++){
    const cJSON*q=cJSON_GetArrayItem(fields,i);pet_form_field_t*p=&form.fields[i];
    const char*id=pet_form_string(q,"id"),*name=pet_form_string(q,"var"),*draft=pet_form_string(q,"draft_var"),*kind=pet_form_string(q,"kind");bool is_text=!strcmp(kind,"text");
    int a=pet_form2_var(vars,name,is_text?"string":"int"),b=pet_form2_var(vars,draft,is_text?"string":"int");
    if(!pet_form_id(id,15)||a<0||b<0||a==b||(owned&((1U<<a)|(1U<<b))))return false;
    for(int j=0;j<i;j++)if(!strcmp(form.fields[j].id,id))return false;
    strcpy(p->id,id);p->var=a;p->draft_var=b;owned|=(1U<<a)|(1U<<b);ephemeral|=1U<<b;
    if(is_text){
      const char*const keys[]={"id","var","draft_var","kind","max_bytes"};int32_t max;
      if(!pet_form_keys(q,keys,5)||!pet_form_integer(pet_form_get(q,"max_bytes"),&max)||max<1||max>63||!pet_form_utf8(initial[a].text,max,NULL,NULL,NULL)||strcmp(initial[a].text,initial[b].text)
          ||pet_form_get(pet_form_get(root,"limits"),name)||pet_form_get(pet_form_get(root,"limits"),draft))return false;
      text.text_mask|=1U<<i;text.max_bytes[i]=max;
    }else if(!strcmp(kind,"integer")){
      const char*const keys[]={"id","var","draft_var","kind","min","max","step"};
      if(!pet_form_keys(q,keys,7)||!pet_form_integer(pet_form_get(q,"min"),&p->min)||!pet_form_integer(pet_form_get(q,"max"),&p->max)||!pet_form_integer(pet_form_get(q,"step"),&p->step)||p->min>=p->max||p->step<1||((int64_t)p->max-p->min)%p->step)return false;
    }else if(!strcmp(kind,"choice")){
      const char*const keys[]={"id","var","draft_var","kind","options"};const cJSON*options=pet_form_get(q,"options");
      if(!pet_form_keys(q,keys,5)||!cJSON_IsArray(options)||cJSON_GetArraySize(options)<2||cJSON_GetArraySize(options)>8)return false;
      p->option_count=cJSON_GetArraySize(options);
      for(int j=0;j<p->option_count;j++){const cJSON*o=cJSON_GetArrayItem(options,j);const char*const keys[]={"value","label"};const char*label=pet_form_string(o,"label");unsigned n;
        if(!pet_form_keys(o,keys,2)||!pet_form_integer(pet_form_get(o,"value"),&p->values[j])||!pet_form_utf8(label,63,&n,NULL,NULL)||!n)return false;
        strcpy(p->labels[j],label);if(!j||p->values[j]<p->min)p->min=p->values[j];if(!j||p->values[j]>p->max)p->max=p->values[j];}
    }else return false;
    if(!is_text&&(!pet_form_value_valid(p,initial[a].number)||initial[a].number!=initial[b].number||!pet_form_range(root,name,p->min,p->max)||!pet_form_range(root,draft,p->min,p->max)))return false;
  }
  const cJSON*k=pet_form_get(f,"keyboard"),*chars=pet_form_get(k,"characters");bool has_space=pet_form_get(k,"space_label")!=NULL;
  const char*const kk[]={"index_var","label_var","characters","space_label"};
  if(!pet_form_keys(k,kk,has_space?4:3)||!cJSON_IsArray(chars)||cJSON_GetArraySize(chars)<1||cJSON_GetArraySize(chars)>(int)PET_FORM_CHARACTERS)return false;
  int ki=pet_form_var(vars,pet_form_string(k,"index_var")),kl=pet_form2_var(vars,pet_form_string(k,"label_var"),"string");
  if(ki<0||kl<0||ki==kl||(owned&((1U<<ki)|(1U<<kl)))||initial[ki].number||!pet_form_range(root,pet_form_string(k,"index_var"),0,cJSON_GetArraySize(chars)-1)||pet_form_get(pet_form_get(root,"limits"),pet_form_string(k,"label_var")))return false;
  text.index_var=ki;text.label_var=kl;text.character_count=cJSON_GetArraySize(chars);owned|=(1U<<ki)|(1U<<kl);ephemeral|=(1U<<ki)|(1U<<kl);
  for(int i=0;i<text.character_count;i++){const cJSON*c=cJSON_GetArrayItem(chars,i);unsigned count;if(!cJSON_IsString(c)||!pet_form_utf8(c->valuestring,4,NULL,&count,NULL)||count!=1)return false;strcpy(text.characters[i],c->valuestring);}
  const char*space=has_space?pet_form_string(k,"space_label"):"Space";unsigned length;if(!pet_form_utf8(space,16,&length,NULL,NULL)||!length)return false;strcpy(text.space_label,space);
  const char*selected=!strcmp(text.characters[0]," ")?space:text.characters[0];if(strcmp(initial[kl].text,selected))return false;
  const char*const ops[]={"previous","next","decrease","increase","accept","cancel","append","erase"};
  const char*const reserved[]={"page_toggle","page_enter","page_back","page_main","page_app","component_center"};
  const char*const physical[]={"button.sw1.short_press","button.sw2.short_press","button.encoder.short_press","button.encoder.long_press","knob.rotate_ccw","knob.rotate_cw","joystick.up","joystick.down"};
  const cJSON*actions=pet_form_get(f,"actions"),*rules=pet_form_get(root,"transitions");
  if(!pet_form_keys(actions,ops,8)||!cJSON_IsArray(rules)||!cJSON_IsArray(buttons)||cJSON_GetArraySize(buttons)!=8)return false;
  cJSON_ArrayForEach(v,buttons){bool known=false;for(unsigned i=0;i<8;i++)known|=!strcmp(pet_form_string(v,"event"),physical[i]);if(!known)return false;
    for(const cJSON*p=buttons->child;p!=v;p=p->next)if(!strcmp(pet_form_string(v,"event"),pet_form_string(p,"event"))||!strcmp(pet_form_string(v,"action"),pet_form_string(p,"action")))return false;}
  for(unsigned i=0;i<8;i++){
    const char*a=pet_form_string(actions,ops[i]);if(!pet_form_id(a,47)||!strncmp(a,"host.",5)||!strncmp(a,"input.",6))return false;
    for(unsigned j=0;j<6;j++)if(!strcmp(a,reserved[j]))return false;
    strcpy(i<6?form.actions[i]:text.extra_actions[i-6],a);
    int count=0,bound=0;cJSON_ArrayForEach(v,rules)if(!strcmp(pet_form_string(v,"on"),a)){const char*const keys[]={"from","on"};if(!pet_form_keys(v,keys,2)||strcmp(pet_form_string(v,"from"),"*"))return false;count++;}
    cJSON_ArrayForEach(v,buttons)bound+=!strcmp(pet_form_string(v,"action"),a);
    if(count!=1||bound!=1||pet_form_contains(pet_form_get(input,"press"),a)||!strcmp(pet_form_string(input,"hold"),a)||pet_form_get(pet_form_get(input,"confirm"),a)||pet_form_get(pet_form_get(input,"cooldown"),a)||(i>=4&&pet_form_contains(pet_form_get(input,"repeat"),a)))return false;
  }
  cJSON_ArrayForEach(v,rules)if(!pet_form2_safe(v,vars,owned)||!pet_form2_safe(pet_form_get(v,"else"),vars,owned))return false;
  cJSON_ArrayForEach(v,pet_form_get(root,"tick"))if(!pet_form2_safe(v,vars,owned)||!pet_form2_safe(pet_form_get(v,"then"),vars,owned))return false;
  cJSON_ArrayForEach(v,pet_form_get(pet_form_get(root,"persist"),"vars")){int i=cJSON_IsString(v)?pet_form2_var(vars,v->valuestring,"int"):-1;if(i<0&&cJSON_IsString(v))i=pet_form2_var(vars,v->valuestring,"string");if(i>=0&&(ephemeral&(1U<<i)))return false;}
  cJSON_ArrayForEach(v,pet_form_get(s,"elements"))if(pet_form_get(v,"field_bind")){const char*type=pet_form_string(v,"type");if((strcmp(type,"text")&&strcmp(type,"button"))||pet_form_field_index(&form,pet_form_string(v,"field_bind"))<0||pet_form_get(v,"text")||pet_form_get(v,"bind")||pet_form_get(v,"var_bind")||pet_form_get(v,"key_for"))return false;}
  if(!pet_form2_valid(&form,&text))return false;
  *out=form;*text_out=text;return true;
}
