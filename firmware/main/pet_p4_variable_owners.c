/* Shared trusted data-only parser; no execution or resource grants. */
#include "pet_p4_variable_owners.h"
#if PET_VARIABLE_OWNERS_ENABLED
static inline bool pet_variable_owners_name(const char *s){
  if(!s||!s[0]||strlen(s)>31)return false;
  for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='.'||*s=='_'||*s=='-'))return false;
  return true;
}
static inline int pet_variable_owners_find(const cJSON *vars,const char *name){
  if(!name)return -1;int at=0;const cJSON *v;
  cJSON_ArrayForEach(v,vars){if(v->string&&!strcmp(name,v->string))return at;at++;}
  return -1;
}
static inline bool pet_variable_owners_ref(pet_variable_owners_t plan,const cJSON *vars,const cJSON *v){
  return cJSON_IsString(v)&&pet_variable_owners_host(plan,pet_variable_owners_find(vars,v->valuestring));
}
static inline bool pet_variable_owners_string(const cJSON *o,const char *key,const char *value){
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
  return cJSON_IsString(v)&&!strcmp(v->valuestring,value);
}
/* Native transitions/ticks/dashboard currently resolve only the native model.
 * Reject host dependencies here, not by silently reading its initial value.
 * Explicit program inputs and surface bindings have separate snapshot paths.
 * Runtime JSON depth is bounded separately; this walk remains finite itself. */
static inline bool pet_variable_owners_native_refs(pet_variable_owners_t plan,const cJSON *vars,const cJSON *o,unsigned depth){
  if(!o)return true;if(depth>16)return false;
  if(!cJSON_IsObject(o)&&!cJSON_IsArray(o))return true;
  const cJSON *v;cJSON_ArrayForEach(v,o){
    if(v->string&&(!strcmp(v->string,"var")||!strcmp(v->string,"fmt_mmss")||!strcmp(v->string,"fmt_hms")
        ||!strcmp(v->string,"pct_of")||!strcmp(v->string,"of_max"))&&pet_variable_owners_ref(plan,vars,v))return false;
    if(!pet_variable_owners_native_refs(plan,vars,v,depth+1))return false;
  }
  return true;
}
/* Output changes only on success. Legacy absence preserves all-host/native
 * behavior; a present unsupported declaration is rejected, never ignored. */
bool pet_variable_owners_parse(const cJSON *root,pet_variable_owners_t *out){
  if(!root||!out)return false;
  const cJSON *vars=cJSON_GetObjectItemCaseSensitive(root,"vars"),*p=cJSON_GetObjectItemCaseSensitive(root,"ownership");
  int n=cJSON_GetArraySize(vars);if(!cJSON_IsObject(vars)||n>8)return false;
  bool hosted=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"host"));
  pet_variable_owners_t plan={.host_mask=hosted?(uint8_t)((1U<<(unsigned)n)-1):0};
  if(!p){*out=plan;return true;}
  if(!hosted||!n||!cJSON_IsObject(p)||cJSON_GetArraySize(p)!=2)return false;
  if(!pet_variable_owners_string(root,"engine","p4-bounded-runtime-v4")
      ||!pet_variable_owners_string(root,"host_context","p4-host-context-v1")
      ||!pet_variable_owners_string(cJSON_GetObjectItemCaseSensitive(root,"input"),"engine","p4-input-v1")
      ||!pet_variable_owners_string(cJSON_GetObjectItemCaseSensitive(root,"surface"),"engine","p4-app-surface-v1")
      ||!pet_variable_owners_string(cJSON_GetObjectItemCaseSensitive(root,"surface"),"bindings","p4-variable-bindings-v1"))return false;
  unsigned engine_keys=0,host_keys=0;const cJSON *v;
  cJSON_ArrayForEach(v,p){
    if(v->string&&!strcmp(v->string,"engine"))engine_keys++;
    else if(v->string&&!strcmp(v->string,"host"))host_keys++;
    else return false;
  }
  const cJSON *engine=cJSON_GetObjectItemCaseSensitive(p,"engine"),*host=cJSON_GetObjectItemCaseSensitive(p,"host");
  if(engine_keys!=1||host_keys!=1||!cJSON_IsString(engine)||strcmp(engine->valuestring,PET_VARIABLE_OWNERS_ENGINE)
      ||!cJSON_IsArray(host)||cJSON_GetArraySize(host)<1||cJSON_GetArraySize(host)>n)return false;
  const char *forbidden[]={"local","scene","game","media","instrument","data"};
  for(unsigned i=0;i<6;i++)if(cJSON_HasObjectItem(root,forbidden[i]))return false;
  const char *objects[]={"form","persist","program","dashboard"};
  for(unsigned i=0;i<4;i++){
    const cJSON *o=cJSON_GetObjectItemCaseSensitive(root,objects[i]);if(o&&!cJSON_IsObject(o))return false;
  }
  int index=0;
  cJSON_ArrayForEach(v,vars){
    if(!pet_variable_owners_name(v->string)||pet_variable_owners_find(vars,v->string)!=index++)return false;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(v,"type");
    if(!cJSON_IsString(type)||(strcmp(type->valuestring,"int")&&strcmp(type->valuestring,"string")))return false;
  }
  plan=(pet_variable_owners_t){.mixed=true};
  cJSON_ArrayForEach(v,host){
    if(!cJSON_IsString(v)||!pet_variable_owners_name(v->valuestring))return false;
    int at=pet_variable_owners_find(vars,v->valuestring);
    if(at<0||pet_variable_owners_host(plan,at))return false;
    plan.host_mask|=(uint8_t)(1U<<(unsigned)at);
  }
  const char *lists[]={"transitions","tick"};
  for(unsigned i=0;i<2;i++){
    const cJSON *rules=cJSON_GetObjectItemCaseSensitive(root,lists[i]);
    if(rules&&!cJSON_IsArray(rules))return false;
    const cJSON *rule;cJSON_ArrayForEach(rule,rules){
      const cJSON *then=cJSON_GetObjectItemCaseSensitive(rule,"then");
      if(!cJSON_IsObject(rule)||(then&&!cJSON_IsObject(then)))return false;
      const cJSON *on=cJSON_GetObjectItemCaseSensitive(rule,"on");
      if(i==0&&cJSON_IsString(on)&&!strncmp(on->valuestring,"host.",5)){
        const cJSON *from=cJSON_GetObjectItemCaseSensitive(rule,"from");
        if(cJSON_GetArraySize(rule)!=2||!cJSON_IsString(from)||strcmp(from->valuestring,"*"))return false;
      }
      if(!pet_variable_owners_native_refs(plan,vars,rule,0))return false;
      const cJSON *bodies[]={rule,cJSON_GetObjectItemCaseSensitive(rule,"then")};
      for(unsigned b=0;b<2;b++)for(unsigned op=0;op<2;op++){
        const cJSON *writes=cJSON_GetObjectItemCaseSensitive(bodies[b],op?"inc":"set");
        if(writes&&!cJSON_IsObject(writes))return false;
        cJSON_ArrayForEach(v,writes)if(pet_variable_owners_host(plan,pet_variable_owners_find(vars,v->string)))return false;
      }
    }
  }
  if(!pet_variable_owners_native_refs(plan,vars,cJSON_GetObjectItemCaseSensitive(root,"dashboard"),0))return false;
  const cJSON *persist=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"persist"),"vars");
  if(persist&&!cJSON_IsArray(persist))return false;
  cJSON_ArrayForEach(v,persist)if(pet_variable_owners_ref(plan,vars,v))return false;
  const cJSON *form=cJSON_GetObjectItemCaseSensitive(root,"form");
  const char *form_keys[]={"focus_var","active_var"},*keyboard_keys[]={"index_var","label_var"};
  for(unsigned i=0;i<2;i++)if(pet_variable_owners_ref(plan,vars,cJSON_GetObjectItemCaseSensitive(form,form_keys[i])))return false;
  const cJSON *keyboard=cJSON_GetObjectItemCaseSensitive(form,"keyboard");
  if(keyboard&&!cJSON_IsObject(keyboard))return false;
  for(unsigned i=0;i<2;i++)if(pet_variable_owners_ref(plan,vars,cJSON_GetObjectItemCaseSensitive(keyboard,keyboard_keys[i])))return false;
  const cJSON *fields=cJSON_GetObjectItemCaseSensitive(form,"fields");if(fields&&!cJSON_IsArray(fields))return false;
  const cJSON *field;cJSON_ArrayForEach(field,fields){
    if(!cJSON_IsObject(field))return false;
    if(pet_variable_owners_ref(plan,vars,cJSON_GetObjectItemCaseSensitive(field,"var"))
        ||pet_variable_owners_ref(plan,vars,cJSON_GetObjectItemCaseSensitive(field,"draft_var")))return false;
  }
  const cJSON *program=cJSON_GetObjectItemCaseSensitive(root,"program");
  if(program){
    const cJSON *outputs=cJSON_GetObjectItemCaseSensitive(program,"outputs");
    if(!cJSON_IsArray(outputs)||cJSON_GetArraySize(outputs)!=8)return false;
    cJSON_ArrayForEach(v,outputs)if(pet_variable_owners_ref(plan,vars,v))return false;
  }
  *out=plan;return true;
}

#endif
