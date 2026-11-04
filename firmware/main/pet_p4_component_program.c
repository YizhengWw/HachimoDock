#include "pet_p4_component_program.h"
#include "pet_p4_variable_owners.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>
#include <limits.h>
#if PET_COMPONENT_PROGRAM_ENABLED
static bool fields(const cJSON *o,const char *const *names,unsigned count){
  if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=(int)count)return false;
  for(unsigned i=0;i<count;i++){
    unsigned found=0;const cJSON *v;cJSON_ArrayForEach(v,o)if(!strcmp(v->string,names[i]))found++;
    if(found!=1)return false;
  }return true;
}
static const char *string(const cJSON *o,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static bool number(const cJSON *v,int64_t lo,int64_t hi,int64_t *out){
  if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||floor(v->valuedouble)!=v->valuedouble||v->valuedouble<(double)lo||v->valuedouble>(double)hi)return false;
  *out=(int64_t)v->valuedouble;return true;
}
static bool name(const char *s,unsigned max){
  if(!s||!s[0]||strlen(s)>max)return false;
  for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='.'||*s=='_'||*s=='-'))return false;
  return true;
}
static int variable(const cJSON *vars,const char *s){
  int i=0;const cJSON *v;cJSON_ArrayForEach(v,vars){if(v->string&&!strcmp(v->string,s))return i;i++;}return -1;
}
static bool instruction_image(const cJSON *p,pet_program_image_t *out,bool v2,const cJSON *root){
  static const char *const keys[]={"engine","schema","cells","fuel","code"};
  static const char *const ops[]={"HALT","CONST","MOVE","ADD","SUB","MUL","DIV","MOD","LT","EQ","JUMP","JZ","LOAD","STORE","EVENT","OUTPUT",
    /* v2 image opcodes (index 16.. maps to PET_OP_TEXT..); INTENT is never nameable. */"TEXT","CELLGET","CELLSET"};
  int64_t n;
  /* A v1 image keeps the v1 budgets and op table even under a v2 build; the v2 image engine needs program engine v2. */
  bool image_v2=v2&&!strcmp(string(p,"engine"),PET_COMPONENT_PROGRAM_IMAGE_ENGINE_V2);
  int64_t max_cells=image_v2?(int64_t)PET_PROGRAM_CELLS:(int64_t)PET_PROGRAM_V1_CELLS,max_fuel=image_v2?(int64_t)PET_PROGRAM_FUEL:(int64_t)PET_PROGRAM_V1_FUEL;
  int max_code=image_v2?(int)PET_PROGRAM_CODE:(int)PET_PROGRAM_V1_CODE;unsigned op_count=image_v2?19:16;
  if(!fields(p,keys,5)||(!image_v2&&strcmp(string(p,"engine"),PET_COMPONENT_PROGRAM_IMAGE_ENGINE))
      ||!number(cJSON_GetObjectItemCaseSensitive(p,"schema"),1,UINT32_MAX,&n))return false;
  out->schema=(uint32_t)n;
  if(!number(cJSON_GetObjectItemCaseSensitive(p,"cells"),0,max_cells,&n))return false;out->cells=(pet_program_cells_t)n;
  if(!number(cJSON_GetObjectItemCaseSensitive(p,"fuel"),1,max_fuel,&n))return false;out->fuel=(uint16_t)n;
#if PET_P4_PROGRAM_V2
  /* Package-level cells/texts feed the VM descriptor; the image bytes never carry them. */
  const cJSON *grid=cJSON_GetObjectItemCaseSensitive(root,"cells"),*texts=cJSON_GetObjectItemCaseSensitive(root,"texts");
  if(grid){int64_t cols,rows;if(!image_v2||!number(cJSON_GetObjectItemCaseSensitive(grid,"cols"),1,16,&cols)||!number(cJSON_GetObjectItemCaseSensitive(grid,"rows"),1,16,&rows)||cols*rows>out->cells)return false;out->grid_cols=(uint8_t)cols;out->grid_rows=(uint8_t)rows;}
  if(texts){if(!image_v2||!cJSON_IsArray(texts)||cJSON_GetArraySize(texts)<1||cJSON_GetArraySize(texts)>64)return false;out->texts=(uint8_t)cJSON_GetArraySize(texts);}
#else
  (void)root;
#endif
  const cJSON *code=cJSON_GetObjectItemCaseSensitive(p,"code");
  if(!cJSON_IsArray(code)||cJSON_GetArraySize(code)<1||cJSON_GetArraySize(code)>max_code)return false;
  out->count=(uint16_t)cJSON_GetArraySize(code);
  for(unsigned i=0;i<out->count;i++){
    const cJSON *row=cJSON_GetArrayItem(code,i),*op=cJSON_GetArrayItem(row,0);
    if(!cJSON_IsArray(row)||cJSON_GetArraySize(row)!=5||!cJSON_IsString(op))return false;
    unsigned index=0;for(;index<op_count;index++)if(!strcmp(op->valuestring,ops[index]))break;
    if(index==op_count)return false;
    pet_program_insn_t *insn=&out->code[i];insn->op=(uint8_t)(index<16?index:index+1);
    uint8_t *abc[]={&insn->a,&insn->b,&insn->c};
    for(unsigned k=0;k<3;k++){if(!number(cJSON_GetArrayItem(row,k+1),0,255,&n))return false;*abc[k]=(uint8_t)n;}
    if(!number(cJSON_GetArrayItem(row,4),INT32_MIN,INT32_MAX,&n))return false;insn->imm=(int32_t)n;
  }
  pet_program_t value={out->code,out->count,out->fuel,out->cells,0
#if PET_P4_PROGRAM_V2
    ,out->grid_cols,out->grid_rows,out->texts
#endif
  };
  out->valid=pet_program_validate(&value)==PET_PROGRAM_READY;return out->valid;
}
static bool owns(const pet_component_program_config_t *c,const cJSON *vars,const char *s){
  int v=variable(vars,s);if(v<0)return false;
  for(unsigned i=0;i<8;i++)if(c->projection.target[i]==v)return true;
  return false;
}
static bool other_writers(const cJSON *root,const cJSON *vars,const pet_component_program_config_t *c){
  const char *lists[]={"transitions","tick"};
  for(unsigned k=0;k<2;k++){
    const cJSON *rule;cJSON_ArrayForEach(rule,cJSON_GetObjectItemCaseSensitive(root,lists[k])){
      const cJSON *bodies[]={rule,cJSON_GetObjectItemCaseSensitive(rule,"then"),cJSON_GetObjectItemCaseSensitive(rule,"else")};
      for(unsigned b=0;b<3;b++)for(unsigned op=0;op<2;op++){
        const cJSON *entry;cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(bodies[b],op?"inc":"set"))
          if(owns(c,vars,entry->string))return true;
      }
    }
  }
  const cJSON *entry;
  cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"persist"),"vars"))
    if(!cJSON_IsString(entry)||owns(c,vars,entry->valuestring))return true;
  const cJSON *form=cJSON_GetObjectItemCaseSensitive(root,"form");
  const char *form_writes[]={"focus_var","active_var"};
  for(unsigned k=0;k<2;k++)if(owns(c,vars,string(form,form_writes[k])))return true;
  const cJSON *keyboard=cJSON_GetObjectItemCaseSensitive(form,"keyboard");
  if(owns(c,vars,string(keyboard,"index_var"))||owns(c,vars,string(keyboard,"label_var")))return true;
  cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(form,"fields"))
    if(owns(c,vars,string(entry,"var"))||owns(c,vars,string(entry,"draft_var")))return true;
  return false;
}
#endif
bool pet_component_program_parse(const cJSON *root,pet_component_program_config_t *out){
  if(!out)return false;memset(out,0,sizeof(*out));
  const cJSON *p=cJSON_GetObjectItemCaseSensitive(root,"program");if(!p)return true;
#if !PET_COMPONENT_PROGRAM_ENABLED
  return false;
#else
  static const char *const keys[]={"engine","image","outputs","actions","timers"};
  out->v2=PET_P4_PROGRAM_V2&&!strcmp(string(p,"engine"),PET_COMPONENT_PROGRAM_ENGINE_V2);
  if(!fields(p,keys,5)||(!out->v2&&strcmp(string(p,"engine"),PET_COMPONENT_PROGRAM_ENGINE))
      ||strcmp(string(root,"engine"),"p4-bounded-runtime-v4")||strcmp(string(cJSON_GetObjectItemCaseSensitive(root,"surface"),"engine"),"p4-app-surface-v1")
      ||strcmp(string(cJSON_GetObjectItemCaseSensitive(root,"input"),"engine"),"p4-input-v1"))return false;
  pet_variable_owners_t owners;
  if(!pet_variable_owners_parse(root,&owners)||(cJSON_HasObjectItem(root,"host")&&!owners.mixed))return false;
  const char *exclusive[]={"local","scene","game","media","instrument","data"};
  for(unsigned i=0;i<6;i++)if(cJSON_HasObjectItem(root,exclusive[i]))return false;
  if(!instruction_image(cJSON_GetObjectItemCaseSensitive(p,"image"),&out->image,out->v2,root))return false;
  const cJSON *vars=cJSON_GetObjectItemCaseSensitive(root,"vars");
  if(!cJSON_IsObject(vars)||cJSON_GetArraySize(vars)<1||cJSON_GetArraySize(vars)>8)return false;
  out->projection.schema=out->image.schema;out->projection.variables=(uint8_t)cJSON_GetArraySize(vars);
  memset(out->projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(out->projection.target));
  unsigned index=0;const cJSON *v;
  cJSON_ArrayForEach(v,vars){
    if(!name(v->string,31)||variable(vars,v->string)!=(int)index)return false;
    if(!strcmp(string(v,"type"),"int")){
      out->projection.minimum[index]=-1000000000;out->projection.maximum[index]=1000000000;
      const cJSON *limits=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"limits"),v->string);
      if(limits){int64_t lo,hi;if(!cJSON_IsArray(limits)||cJSON_GetArraySize(limits)!=2||!number(cJSON_GetArrayItem(limits,0),-1000000000,1000000000,&lo)||!number(cJSON_GetArrayItem(limits,1),lo,1000000000,&hi))return false;
        out->projection.minimum[index]=(int32_t)lo;out->projection.maximum[index]=(int32_t)hi;}
    }else if(strcmp(string(v,"type"),"string"))return false;
    index++;
  }
  const cJSON *outputs=cJSON_GetObjectItemCaseSensitive(p,"outputs");
  if(!cJSON_IsArray(outputs)||cJSON_GetArraySize(outputs)!=8)return false;
  uint16_t used=0;
  for(unsigned i=0;i<8;i++){
    const cJSON *slot=cJSON_GetArrayItem(outputs,i);if(cJSON_IsNull(slot))continue;
    if(!cJSON_IsString(slot)||!name(slot->valuestring,31))return false;
    int at=variable(vars,slot->valuestring);const cJSON *def=cJSON_GetObjectItemCaseSensitive(vars,slot->valuestring);
    if(at<0||strcmp(string(def,"type"),"int")||(used&(1u<<at)))return false;
    used|=1u<<at;out->projection.target[i]=(uint8_t)at;
  }
  if(!used||other_writers(root,vars,out))return false;
  const cJSON *actions=cJSON_GetObjectItemCaseSensitive(p,"actions");
  if(!cJSON_IsObject(actions)||cJSON_GetArraySize(actions)<1||cJSON_GetArraySize(actions)>(int)PET_COMPONENT_PROGRAM_ACTIONS)return false;
  const cJSON *action;cJSON_ArrayForEach(action,actions){
    if(!name(action->string,47)||!strncmp(action->string,"host.",5))return false;
    const cJSON *form_action;cJSON_ArrayForEach(form_action,cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"form"),"actions"))
      if(cJSON_IsString(form_action)&&!strcmp(form_action->valuestring,action->string))return false;
    for(unsigned i=0;i<out->action_count;i++)if(!strcmp(action->string,out->actions[i].name))return false;
    bool declared=false;const cJSON *t;
    cJSON_ArrayForEach(t,cJSON_GetObjectItemCaseSensitive(root,"transitions"))if(!strcmp(action->string,string(t,"on"))){
      static const char *const fields_only[]={"from","on"};
      if(!fields(t,fields_only,2)||strcmp(string(t,"from"),"*"))return false;declared=true;
    }
    if(!declared||!cJSON_IsArray(action)||cJSON_GetArraySize(action)>4)return false;
    unsigned at=out->action_count++;strcpy(out->actions[at].name,action->string);
    out->actions[at].event.count=(uint8_t)cJSON_GetArraySize(action);memset(out->actions[at].input,-1,4);
    for(unsigned k=0;k<out->actions[at].event.count;k++){
      const cJSON *word=cJSON_GetArrayItem(action,k);int64_t n;
      if(number(word,INT32_MIN,INT32_MAX,&n))out->actions[at].event.words[k]=(int32_t)n;
      else {static const char *const var_only[]={"var"};const char *s=string(word,"var");int vi=variable(vars,s);
        if(!fields(word,var_only,1)||vi<0||strcmp(string(cJSON_GetObjectItemCaseSensitive(vars,s),"type"),"int"))return false;
        out->actions[at].input[k]=(int8_t)vi;}
    }
  }
  const cJSON *timers=cJSON_GetObjectItemCaseSensitive(p,"timers");
  if(!cJSON_IsArray(timers)||cJSON_GetArraySize(timers)>8)return false;
  out->timer_count=(uint8_t)cJSON_GetArraySize(timers);
  for(unsigned i=0;i<out->timer_count;i++){
    static const char *const timer_keys[]={"id","delayMs","intervalMs","words"};
    const cJSON *t=cJSON_GetArrayItem(timers,i);pet_program_timer_spec_t *spec=&out->timers[i];int64_t n;
    if(!fields(t,timer_keys,4)||!number(cJSON_GetObjectItemCaseSensitive(t,"id"),1,65535,&n))return false;spec->id=(uint16_t)n;
    for(unsigned k=0;k<i;k++)if(out->timers[k].id==spec->id)return false;
    if(!number(cJSON_GetObjectItemCaseSensitive(t,"delayMs"),0,UINT32_MAX,&n))return false;spec->delay_ms=(uint32_t)n;
    if(!number(cJSON_GetObjectItemCaseSensitive(t,"intervalMs"),0,UINT32_MAX,&n)||(n&&n<100))return false;spec->interval_ms=(uint32_t)n;
    const cJSON *words=cJSON_GetObjectItemCaseSensitive(t,"words");if(!cJSON_IsArray(words)||cJSON_GetArraySize(words)>4)return false;
    spec->event.count=(uint8_t)cJSON_GetArraySize(words);
    for(unsigned k=0;k<spec->event.count;k++){if(!number(cJSON_GetArrayItem(words,k),INT32_MIN,INT32_MAX,&n))return false;spec->event.words[k]=(int32_t)n;}
  }
  out->enabled=true;return true;
#endif
}
#if PET_COMPONENT_PROGRAM_ENABLED
bool pet_component_program_seed_valid(const pet_component_program_config_t *c,const pet_program_snapshot_t *seed){
  if(!c||!c->enabled||!seed||c->image.cells>PET_PROGRAM_CELLS
      ||c->projection.schema!=c->image.schema||c->projection.variables<1||c->projection.variables>8
      ||seed->schema!=c->image.schema||seed->cells!=c->image.cells)return false;
  for(unsigned i=seed->cells;i<PET_PROGRAM_CELLS;i++)if(seed->values[i])return false;
  for(unsigned i=0;i<8;i++){unsigned at=c->projection.target[i];if(at!=PET_PROGRAM_UI_UNBOUND
      &&(at>=8||at>=c->projection.variables||c->projection.minimum[at]>c->projection.maximum[at]
        ||seed->outputs[i]<c->projection.minimum[at]||seed->outputs[i]>c->projection.maximum[at]))return false;}
  return true;
}
bool pet_component_program_event(const pet_component_program_config_t *c,const char *action,
    const int32_t values[8],pet_program_event_t *out){
  if(!c||!c->enabled||!action||!values||!out||c->action_count>PET_COMPONENT_PROGRAM_ACTIONS)return false;
  for(unsigned i=0;i<c->action_count;i++)if(!strcmp(c->actions[i].name,action)){
    pet_program_event_t next=c->actions[i].event;
    if(next.count>PET_PROGRAM_EVENT_WORDS)return false;
    for(unsigned k=0;k<next.count;k++){int vi=c->actions[i].input[k];if(vi>=0){if(vi>=c->projection.variables)return false;next.words[k]=values[vi];}}
    *out=next;return true;
  }return false;
}
#endif
