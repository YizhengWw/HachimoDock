/* Bounded request/reply transport. Never retransmit an action on timeout. */
#include "pet_p4_app_host.h"
#include "pet_p4_host_observation.h"
#include "pet_p4_host_render.h"
#include "pet_p4_resources_device.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "pet_p4_service_bridge_json.h"
#if PET_SERVICE_BRIDGE_ENABLED
#include "esp_attr.h"
#include "mbedtls/base64.h"
#define BRIDGE_ARG(x) ,x
#else
#define BRIDGE_ARG(x)
#endif
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static pet_p4_send_line_fn transport;
static void *transport_ctx;
static char board_id[PET_P4_DEVICE_ID_MAX];
static char widget[48],instance[32],values[8][160];
static uint32_t generation,sequence,applied,incarnation;
static uint64_t last_poll,last_reply,last_action;
#if PET_VARIABLE_OWNERS_ENABLED
/* A pending command may refresh legacy status text, never the age of data
 * consumed by a mixed native program. Only an applied reply publishes data. */
static uint64_t variable_reply_at;
/* One 32-byte native scratch, accessed ONLY while mux is held. Unlike a task
 * stack candidate, this does not grow the HOST/input/form call chain. Nothing
 * can observe it before the all-or-none output copy under the same mux. */
static int32_t owned_integer_values[8];
#endif
static bool shown;
static bool ui_valid,ui_pending;
#if PET_VARIABLE_OWNERS_ENABLED
static bool owned_fresh_locked(uint64_t now){
  return !ui_pending&&variable_reply_at&&now>=variable_reply_at&&now-variable_reply_at<10000;
}
#endif
static uint8_t ui_mode,ui_count;
static char ui_actions[16][48],ui_context[65];
typedef pet_host_render_variable_t host_variable_t;
static host_variable_t variables[8];
static uint8_t variable_count;
static uint64_t variable_epoch;
#if PET_HOST_COLLECTION_ENABLED
#include "esp_attr.h"
/* Private preparation happens outside mux, never on a task stack. Only the
 * bounded ownership flag and final already-parsed copy use the host mux. */
static EXT_RAM_BSS_ATTR pet_host_collection_snapshot_t collection_view,collection_candidate;
static bool collection_preparing;
static void collection_prepare_end(bool *owned){
  if(!*owned)return;
  portENTER_CRITICAL(&mux);collection_preparing=false;portEXIT_CRITICAL(&mux);
}
#endif
/* Lock order: this host mux -> device resource mux. The broker never calls
 * back into host code and neither section spans transport or physical work. */
static pet_resource_view_session_t resource_view;
static const char *names[]={"eyebrow","headline","metricValue","metricLabel","metricUnit","badge","note","footer"};
static const unsigned sizes[]={96,160,64,96,32,16,160,160};
#if PET_SERVICE_BRIDGE_ENABLED
static EXT_RAM_BSS_ATTR pet_bridge_presentation_t bridge_candidate;
static EXT_RAM_BSS_ATTR pet_bridge_timelines_t bridge_timelines;
static EXT_RAM_BSS_ATTR pet_bridge_images_t bridge_images;
static EXT_RAM_BSS_ATTR uint16_t bridge_pixels[PET_BRIDGE_IMAGES*2*PET_BRIDGE_IMAGE_PIXELS];
static EXT_RAM_BSS_ATTR uint8_t bridge_decoded[2048];
static bool bridge_preparing;
static char active_contract[65];
static void bridge_prepare_end(bool*owned){if(*owned){portENTER_CRITICAL(&mux);bridge_preparing=false;portEXIT_CRITICAL(&mux);}}
#endif
static const char *str(const cJSON *p,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);return cJSON_IsString(v)?v->valuestring:"";}
/* Called only under mux. Revocation does not alter user storage or audio ownership. */
static void clear_view(void){
  pet_resources_device_view_end(&resource_view);
#if PET_SERVICE_BRIDGE_ENABLED
  memset(&bridge_timelines,0,sizeof(bridge_timelines));pet_bridge_images_clear(&bridge_images);active_contract[0]=0;
#endif
  applied=0;last_reply=last_action=0;memset(values,0,sizeof(values));
#if PET_VARIABLE_OWNERS_ENABLED
  variable_reply_at=0;
#endif
  bool changed=variable_count!=0;
#if PET_HOST_COLLECTION_ENABLED
  changed|=collection_view.valid;memset(&collection_view,0,sizeof(collection_view));
#endif
  if(changed)variable_epoch++;
  ui_valid=ui_pending=false;ui_count=0;ui_context[0]=0;variable_count=0;
}
static bool same_variables(const host_variable_t *incoming,int count){
  if(count!=variable_count)return false;
  for(int i=0;i<count;i++){
    const host_variable_t *a=&incoming[i],*b=NULL;
    for(int j=0;j<variable_count;j++)if(!strcmp(a->name,variables[j].name)){b=&variables[j];break;}
    if(!b||a->integer!=b->integer||(a->integer?a->number!=b->number:strcmp(a->text,b->text)!=0))return false;
  }
  return true;
}
void pet_app_host_transport(pet_p4_send_line_fn send,void *ctx,const char *board){
  const char *next=board?board:"";
  portENTER_CRITICAL(&mux);
  if(transport!=send||transport_ctx!=ctx||strcmp(board_id,next)){
    /* Late replies from a replaced transport cannot refresh the former view. */
    shown=false;instance[0]=0;last_poll=0;clear_view();
  }
  transport=send;transport_ctx=ctx;snprintf(board_id,sizeof(board_id),"%s",next);portEXIT_CRITICAL(&mux);
}
/* Caller holds mux. This predicate and reservation below must be atomic with
 * reply publication, not merely checked by an earlier input/render callback. */
static bool enabled_locked(const char *id,const char *action,uint64_t now){
  bool current=shown&&pet_resources_device_view_current(&resource_view)&&!strcmp(id,widget);
  if(!current)return false;
  if(!ui_valid)return true; // Preserve legacy hosts' existing behavior.
  if(ui_pending||!last_reply||now<last_reply||now-last_reply>=10000)return false;
  for(int i=0;i<ui_count;i++)if(!strcmp(action,ui_actions[i]))return true;
  return false;
}
static bool observe_locked(const char *id,pet_host_observation_t *out){
  memset(out,0,sizeof(*out));
  if(!id||!shown||strcmp(id,widget)||!pet_resources_device_view_current(&resource_view))return false;
  out->variable_epoch=variable_epoch;out->incarnation=incarnation;out->revision=generation;
  memcpy(out->component_id,widget,sizeof(widget));memcpy(out->context,ui_context,sizeof(ui_context));out->valid=true;return true;
}
static bool observation_current_locked(const char *id,const pet_host_observation_t *observed){
  pet_host_observation_t current;
  return observed&&observe_locked(id,&current)&&pet_host_observation_equal(observed,&current);
}
static bool request(const char *id,uint32_t rev,uint8_t page,const char *action,uint64_t now,bool visible,const char *confirmed_context,uint32_t expected_incarnation,const pet_host_observation_t *observed BRIDGE_ARG(const cJSON *data) BRIDGE_ARG(const char *binding)){
  char token[32],board[PET_P4_DEVICE_ID_MAX],context[65];uint32_t seq;pet_p4_send_line_fn send;void *ctx;
#if PET_SERVICE_BRIDGE_ENABLED
  char contract_copy[65]={0};
#endif
  uint32_t nonce=esp_random();
  portENTER_CRITICAL(&mux);
  bool command=strcmp(action,"status")!=0;
  if((expected_incarnation&&expected_incarnation!=incarnation)||(command&&(!shown||strcmp(widget,id)||generation!=rev))){portEXIT_CRITICAL(&mux);return false;}
  if(command&&(sequence==UINT32_MAX||!enabled_locked(id,action,now)
      ||(observed&&!observation_current_locked(id,observed)))){portEXIT_CRITICAL(&mux);return false;}
  if(strcmp(widget,id)||generation!=rev||!instance[0]||(!shown&&visible)||sequence==UINT32_MAX
#if PET_SERVICE_BRIDGE_ENABLED
     ||(binding&&strcmp(active_contract,binding))
#endif
  ){
    /* A new visible lifetime never shares a reply token, even at the same
       content revision. Fail closed on the unreachable incarnation wrap. */
    if(incarnation==UINT32_MAX){portEXIT_CRITICAL(&mux);return false;}
    incarnation++;
    snprintf(widget,sizeof(widget),"%s",id);generation=rev;
    snprintf(instance,sizeof(instance),"%08lx-%08lx",(unsigned long)nonce,(unsigned long)incarnation);
    sequence=0;last_poll=0;clear_view();
#if PET_SERVICE_BRIDGE_ENABLED
    if(binding)snprintf(active_contract,sizeof(active_contract),"%s",binding);
#endif
  }
  if(!transport||(!strcmp(action,"status")&&sequence&&now-last_poll<2000&&shown==visible)||(strcmp(action,"status")&&last_action&&now-last_action<150)){
    portEXIT_CRITICAL(&mux);return false;
  }
  if(visible&&!resource_view.owner.generation&&pet_resources_device_view_begin(&resource_view)!=PET_RESOURCE_OK){
    shown=false;portEXIT_CRITICAL(&mux);return false;
  }
  if(visible&&!pet_resources_device_view_current(&resource_view)){
    clear_view();shown=false;portEXIT_CRITICAL(&mux);return false;
  }
  if(!visible)clear_view();
  if(command)last_action=now;
  shown=visible;last_poll=now;seq=++sequence;
  if(command){applied=seq;last_reply=now;
#if PET_VARIABLE_OWNERS_ENABLED
    if(!ui_pending)variable_epoch++;
#endif
    ui_pending=true;snprintf(values[1],sizeof(values[1]),"正在处理");snprintf(values[6],sizeof(values[6]),"已请求电脑，不会重复派发");}
  memcpy(token,instance,sizeof(token));memcpy(board,board_id,sizeof(board));snprintf(context,sizeof(context),"%s",confirmed_context?confirmed_context:observed?observed->context:ui_context);send=transport;ctx=transport_ctx;
#if PET_SERVICE_BRIDGE_ENABLED
  memcpy(contract_copy,active_contract,sizeof(contract_copy));
#endif
  portEXIT_CRITICAL(&mux);
  cJSON *root=cJSON_CreateObject();if(!root)return false;
  cJSON_AddStringToObject(root,"topic","app/event");cJSON *p=cJSON_AddObjectToObject(root,"payload");
  if(p){cJSON_AddStringToObject(p,"boardDeviceId",board);cJSON_AddStringToObject(p,"componentId",id);cJSON_AddStringToObject(p,"instance",token);cJSON_AddNumberToObject(p,"sequence",seq);
    cJSON_AddStringToObject(p,"action",action);cJSON_AddNumberToObject(p,"page",page);cJSON_AddBoolToObject(p,"visible",visible);
    if(command&&context[0])cJSON_AddStringToObject(p,"context",context);
    if(confirmed_context)cJSON_AddBoolToObject(p,"confirmed",true);
#if PET_SERVICE_BRIDGE_ENABLED
    if(contract_copy[0])cJSON_AddStringToObject(p,"contract",contract_copy);
    if(data){cJSON *copy=cJSON_Duplicate(data,true);if(!copy){cJSON_Delete(root);return false;}cJSON_AddItemToObject(p,"data",copy);}
#endif
  }
  char *line=p?cJSON_PrintUnformatted(root):NULL;if(line)send(line,ctx);bool ok=line!=NULL;cJSON_free(line);cJSON_Delete(root);return ok;
}
void pet_app_host_tick(const char *id,uint32_t rev,uint8_t page,bool visible,uint64_t now){
  char previous[48];uint32_t expected_incarnation=0;
  if(!id||!id[0]){
    /* Leaving for a non-host page must revoke the previous host too. Preserve
       its identity for the exit event; the next entry receives a new token. */
    portENTER_CRITICAL(&mux);
    bool notify=shown&&widget[0];snprintf(previous,sizeof(previous),"%s",widget);rev=generation;expected_incarnation=incarnation;
    portEXIT_CRITICAL(&mux);
    if(!notify)return;
    id=previous;visible=false;
  }
  request(id,rev,page,"status",now,visible,NULL,expected_incarnation,NULL BRIDGE_ARG(NULL) BRIDGE_ARG(NULL));
}
bool pet_app_host_action(const char *id,uint32_t rev,uint8_t page,const char *action,uint64_t now){
  if(!id||!id[0]||!action||strncmp(action,"host.",5)||strlen(action)>47)return false;
  if(!pet_app_host_enabled(id,action,now))return true;
  (void)request(id,rev,page,action,now,true,NULL,0,NULL BRIDGE_ARG(NULL) BRIDGE_ARG(NULL));
  return true; /* A throttled host action must not fall through to desktop input. */
}
bool pet_app_host_confirmed_action(const char *id,uint32_t rev,uint8_t page,const char *action,const char *context,uint64_t now){
  if(!id||!action||strncmp(action,"host.",5)||strlen(action)>47||!context||strlen(context)>64)return false;
  if(!pet_app_host_enabled(id,action,now))return true;
  (void)request(id,rev,page,action,now,true,context,0,NULL BRIDGE_ARG(NULL) BRIDGE_ARG(NULL));return true;
}
bool pet_app_host_observe(const char *id,pet_host_observation_t *out){
  if(!out)return false;
  portENTER_CRITICAL(&mux);bool ok=observe_locked(id,out);portEXIT_CRITICAL(&mux);return ok;
}
bool pet_app_host_observation_current(const char *id,const pet_host_observation_t *observed){
  portENTER_CRITICAL(&mux);bool ok=observation_current_locked(id,observed);portEXIT_CRITICAL(&mux);return ok;
}
#if PET_HOST_COLLECTION_ENABLED
bool pet_app_host_collection_observe(const char *id,const pet_host_collection_config_t *config,unsigned slot,
    uint64_t now,pet_host_observation_t *out){
  if(!out)return false;memset(out,0,sizeof(*out));
  portENTER_CRITICAL(&mux);
  bool ok=!ui_pending&&last_reply&&now>=last_reply&&now-last_reply<10000
    &&pet_host_collection_matches(config,&collection_view)&&slot<collection_view.visible
    &&collection_view.rows[slot].present&&enabled_locked(id,config->actions[slot],now)&&observe_locked(id,out);
  portEXIT_CRITICAL(&mux);return ok;
}
#endif
bool pet_app_host_observed_action(const char *id,uint32_t rev,uint8_t page,const char *action,const pet_host_observation_t *observed,uint64_t now){
  if(!id||!id[0]||!action||strncmp(action,"host.",5)||strlen(action)>47)return false;
  /* Missing/stale native observations are consumed, not reinterpreted against
   * the latest rows or downgraded to an unguarded desktop action. */
  if(observed)(void)request(id,rev,page,action,now,true,NULL,0,observed BRIDGE_ARG(NULL) BRIDGE_ARG(NULL));
  return true;
}
int pet_app_host_mode(const char *id,uint64_t now){
  portENTER_CRITICAL(&mux);int mode=!shown||!pet_resources_device_view_current(&resource_view)||strcmp(id,widget)||!last_reply||now-last_reply>=10000?7:ui_pending?6:ui_valid?ui_mode:0;portEXIT_CRITICAL(&mux);return mode;
}
bool pet_app_host_enabled(const char *id,const char *action,uint64_t now){
  portENTER_CRITICAL(&mux);
  bool ok=enabled_locked(id,action,now);
  portEXIT_CRITICAL(&mux);return ok;
}
void pet_app_host_context(const char *id,char context[65]){
  portENTER_CRITICAL(&mux);snprintf(context,65,"%s",shown&&!strcmp(id,widget)?ui_context:"");portEXIT_CRITICAL(&mux);
}
bool pet_app_host_apply(const cJSON *p,uint64_t now){
#if PET_SERVICE_BRIDGE_ENABLED
  bool bridge_owned __attribute__((cleanup(bridge_prepare_end)))=false;
  portENTER_CRITICAL(&mux);if(!bridge_preparing){bridge_preparing=true;bridge_owned=true;}portEXIT_CRITICAL(&mux);
  if(!bridge_owned||!pet_bridge_json_presentation(cJSON_GetObjectItemCaseSensitive(p,"presentation"),&bridge_candidate))return false;
#else
  if(cJSON_HasObjectItem(p,"presentation"))return false;
#endif
  const cJSON *collection=cJSON_GetObjectItemCaseSensitive(p,"collectionView");
#if PET_HOST_COLLECTION_ENABLED
  bool collection_owned __attribute__((cleanup(collection_prepare_end)))=false;
  const pet_host_collection_snapshot_t *incoming_collection=NULL;
  if(collection){
    portENTER_CRITICAL(&mux);
    if(!collection_preparing){collection_preparing=true;collection_owned=true;}
    portEXIT_CRITICAL(&mux);
    if(!collection_owned||!pet_host_collection_parse_snapshot(collection,&collection_candidate))return false;
    incoming_collection=&collection_candidate;
  }
#else
  if(collection)return false;
#endif
  const cJSON *s=cJSON_GetObjectItemCaseSensitive(p,"sequence"),*v=cJSON_GetObjectItemCaseSensitive(p,"values");
  if(!cJSON_IsNumber(s)||!isfinite(s->valuedouble)||s->valuedouble<1||s->valuedouble>UINT32_MAX||floor(s->valuedouble)!=s->valuedouble||!cJSON_IsObject(v))return false;
  host_variable_t incoming[8]={0};int incoming_count=0;
  const cJSON *data=cJSON_GetObjectItemCaseSensitive(p,"variables");
  if(data){
    if(!cJSON_IsObject(data)||cJSON_GetArraySize(data)>8)return false;
    const cJSON *entry;cJSON_ArrayForEach(entry,data){
      const char *name=entry->string;
      if(!name||!name[0]||strlen(name)>31)return false;
      for(const char *c=name;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='_'||*c=='-'))return false;
      for(int i=0;i<incoming_count;i++)if(!strcmp(incoming[i].name,name))return false;
      host_variable_t *target=&incoming[incoming_count++];strcpy(target->name,name);
      if(cJSON_IsNumber(entry)){
        double n=entry->valuedouble;if(!isfinite(n)||floor(n)!=n||n < -1000000000||n > 1000000000)return false;
        target->integer=true;target->number=(int32_t)n;snprintf(target->text,64,"%ld",(long)target->number);
      }else if(cJSON_IsString(entry)){
        if(strlen(entry->valuestring)>63)return false;
        for(const unsigned char *c=(void*)entry->valuestring;*c;c++)if(*c<32||*c==127)return false;
        strcpy(target->text,entry->valuestring);
      }else return false;
    }
  }
#if PET_SERVICE_BRIDGE_ENABLED
  for(unsigned i=0;i<bridge_candidate.timelines.count;i++){
    bool found=false;for(int j=0;j<incoming_count;j++)if(incoming[j].integer&&!strcmp(incoming[j].name,bridge_candidate.timelines.items[i].name))found=true;
    if(!found)return false;
  }
#endif
  const cJSON *field;
  cJSON_ArrayForEach(field,v){
    int index=-1;for(int i=0;i<8;i++)if(!strcmp(field->string,names[i]))index=i;
    if(index<0||!cJSON_IsString(field)||strlen(field->valuestring)>=sizes[index])return false;
    for(const unsigned char *c=(void*)field->valuestring;*c;c++)if(*c<32||*c==127)return false;
  }
  const cJSON *ui=cJSON_GetObjectItemCaseSensitive(p,"ui");
  int mode=0,count=0;char actions[16][48]={{0}},context[65]={0};
  if(ui){
    const cJSON *m=cJSON_GetObjectItemCaseSensitive(ui,"mode"),*a=cJSON_GetObjectItemCaseSensitive(ui,"actions");
    if(!cJSON_IsObject(ui)||!cJSON_IsNumber(m)||m->valuedouble!=m->valueint||m->valueint<0||m->valueint>7||!cJSON_IsArray(a)||cJSON_GetArraySize(a)>16)return false;
    mode=m->valueint;count=cJSON_GetArraySize(a);
    for(int i=0;i<count;i++){const cJSON *s=cJSON_GetArrayItem(a,i);if(!cJSON_IsString(s)||strncmp(s->valuestring,"host.",5)||strlen(s->valuestring)>47)return false;
      for(const char *c=s->valuestring;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='_'||*c=='-'))return false;
      for (int j = 0; j < i; j++) {
        if (!strcmp(actions[j], s->valuestring)) return false;
      }
      snprintf(actions[i],48,"%s",s->valuestring);}
    const cJSON *token=cJSON_GetObjectItemCaseSensitive(ui,"context");
    if(token){if(!cJSON_IsString(token)||strlen(token->valuestring)>64)return false;for(const char *c=token->valuestring;*c;c++)if(!((*c>='0'&&*c<='9')||(*c>='a'&&*c<='f')))return false;snprintf(context,65,"%s",token->valuestring);}
  }
#if PET_HOST_COLLECTION_ENABLED
  if(collection&&(!ui||!context[0]))return false;
#endif
  portENTER_CRITICAL(&mux);
  uint32_t received=(uint32_t)s->valuedouble;
  /* An action reserves applied before its first reply. Once acknowledged,
     duplicates cannot replace values or extend the ten-second freshness lease. */
  bool ok=shown&&pet_resources_device_view_current(&resource_view)&&!strcmp(str(p,"componentId"),widget)&&!strcmp(str(p,"instance"),instance)&&received<=sequence&&(received>applied||(received==applied&&ui_pending))
#if PET_SERVICE_BRIDGE_ENABLED
    &&(!cJSON_HasObjectItem(p,"presentation")||active_contract[0])
    &&(active_contract[0]?!strcmp(active_contract,str(p,"contract")):!cJSON_HasObjectItem(p,"contract"))
#endif
  ;
  if(ok){
    bool changed=!same_variables(incoming,incoming_count);
#if PET_SERVICE_BRIDGE_ENABLED
    bridge_candidate.timelines.received_ms=now;
    changed|=memcmp(&bridge_timelines.items,&bridge_candidate.timelines.items,sizeof(bridge_timelines.items))!=0||bridge_timelines.count!=bridge_candidate.timelines.count;
    bridge_timelines=bridge_candidate.timelines;
    uint64_t prior_epoch=bridge_images.epoch;
    (void)pet_bridge_images_expect(&bridge_images,bridge_candidate.images,bridge_candidate.image_count);
    changed|=prior_epoch!=bridge_images.epoch;
#endif
#if PET_VARIABLE_OWNERS_ENABLED
    changed|=ui_pending||ui_valid!=(ui!=NULL)||ui_mode!=mode||ui_count!=count
      ||memcmp(ui_actions,actions,sizeof(actions))||memcmp(ui_context,context,sizeof(context));
    variable_reply_at=now;
#endif
#if PET_HOST_COLLECTION_ENABLED
    changed|=incoming_collection?memcmp(&collection_view,incoming_collection,sizeof(collection_view))!=0:collection_view.valid;
    if(incoming_collection)collection_view=*incoming_collection;else memset(&collection_view,0,sizeof(collection_view));
#endif
    for(int i=0;i<8;i++)snprintf(values[i],sizeof(values[i]),"%s",str(v,names[i]));
    applied=received;
    last_reply=now;
    ui_valid=ui!=NULL;
    ui_pending=false;
    ui_mode=mode;
    ui_count=count;
    memcpy(ui_actions,actions,sizeof(actions));
    memcpy(ui_context,context,sizeof(context));
    if(changed)variable_epoch++;
    memcpy(variables,incoming,sizeof(variables));
    variable_count=incoming_count;
  }
  portEXIT_CRITICAL(&mux);return ok;
}
uint64_t pet_app_host_variable_epoch(const char *id){
  portENTER_CRITICAL(&mux);uint64_t epoch=id&&!strcmp(id,widget)?variable_epoch:0;portEXIT_CRITICAL(&mux);return epoch;
}
bool pet_app_host_variable(const char *id,const char *name,bool integer,char out[64],int32_t *number,uint64_t now){
  bool found=false;out[0]=0;if(number)*number=0;
  portENTER_CRITICAL(&mux);
  if(shown&&pet_resources_device_view_current(&resource_view)&&!strcmp(id,widget)&&last_reply&&now-last_reply<10000){
    for(int i=0;i<variable_count;i++)if(!strcmp(name,variables[i].name)&&integer==variables[i].integer){
      memcpy(out,variables[i].text,64);if(number)*number=variables[i].number;found=true;break;
    }
  }
  portEXIT_CRITICAL(&mux);return found;
}
bool pet_app_host_integer_snapshot(const char *id,const char names[][32],unsigned count,int32_t out[8],uint64_t now){
  if(!id||!names||!out||count>8)return false;
  int32_t candidate[8]={0};bool ok=true;
  portENTER_CRITICAL(&mux);
  if(!shown||strcmp(id,widget)||!pet_resources_device_view_current(&resource_view)
      ||!last_reply||now<last_reply||now-last_reply>=10000)ok=false;
  for(unsigned i=0;ok&&i<count;i++){
    bool found=false;for(unsigned j=0;j<variable_count;j++)if(variables[j].integer&&!strcmp(names[i],variables[j].name)){candidate[i]=variables[j].number;found=true;break;}
    if(!found)ok=false;
  }
  if(ok)memcpy(out,candidate,sizeof(candidate));
  portEXIT_CRITICAL(&mux);return ok;
}
#if PET_VARIABLE_OWNERS_ENABLED
bool pet_app_host_owned_observation_current(const char *id,const pet_host_observation_t *observed,uint64_t now){
  portENTER_CRITICAL(&mux);
  bool ok=owned_fresh_locked(now)&&observation_current_locked(id,observed);
  portEXIT_CRITICAL(&mux);return ok;
}
static bool owned_integer_snapshot(const char *id,const char names[][32],unsigned count,
    int32_t out[8],const pet_host_observation_t *observed,uint64_t now){
  if(!id||!names||!out||count>8)return false;
  portENTER_CRITICAL(&mux);
  memset(owned_integer_values,0,sizeof(owned_integer_values));
  bool ok=shown&&!strcmp(id,widget)&&pet_resources_device_view_current(&resource_view)
    &&owned_fresh_locked(now)&&(!observed||observation_current_locked(id,observed));
  for(unsigned i=0;ok&&i<count;i++){
    bool found=false;for(unsigned j=0;j<variable_count;j++)if(variables[j].integer&&!strcmp(names[i],variables[j].name)){owned_integer_values[i]=variables[j].number;found=true;break;}
    if(!found)ok=false;
  }
  if(ok)memcpy(out,owned_integer_values,sizeof(owned_integer_values));
  portEXIT_CRITICAL(&mux);return ok;
}
bool pet_app_host_owned_integer_snapshot(const char *id,const char names[][32],unsigned count,int32_t out[8],uint64_t now){
  return owned_integer_snapshot(id,names,count,out,NULL,now);
}
bool pet_app_host_integer_snapshot_observed(const char *id,const char names[][32],unsigned count,
    int32_t out[8],const pet_host_observation_t *observed,uint64_t now){
  return observed&&owned_integer_snapshot(id,names,count,out,observed,now);
}
#endif
/* mux held; no transport, allocation or filesystem work. */
static void render_view_locked(pet_p4_miniapp_view_t *out,bool current,bool fresh){
  char *dest[]={out->eyebrow,out->headline,out->metric_value,out->metric_label,out->metric_unit,out->badge,out->note,out->footer};
  if(!strcmp(out->widget_id,widget)){
    if(current&&fresh){for(int i=0;i<8;i++)snprintf(dest[i],sizes[i],"%s",values[i]);out->revision^=applied;}
    else {snprintf(out->headline,sizeof(out->headline),"等待电脑连接");out->metric_value[0]=0;snprintf(out->note,sizeof(out->note),"请保持 Pet Manager 运行并启用此组件");}
  }
}
void pet_app_host_view(pet_p4_miniapp_view_t *out,uint64_t now){
  if(!out)return;
  portENTER_CRITICAL(&mux);
  bool current=shown&&pet_resources_device_view_current(&resource_view);
  render_view_locked(out,current,last_reply&&now>=last_reply&&now-last_reply<10000);
  portEXIT_CRITICAL(&mux);
}
static void render_capture(const char *id,pet_host_render_t *out,pet_p4_miniapp_view_t *view,uint64_t now,pet_host_observation_t *observed,bool owned){
  (void)owned;
  if(observed)memset(observed,0,sizeof(*observed));
  if(!out)return;
  memset(out,0,sizeof(*out));out->mode=7;
  portENTER_CRITICAL(&mux);
  bool current=id&&shown&&!strcmp(id,widget)&&pet_resources_device_view_current(&resource_view);
  if(observed)observe_locked(id,observed);
  out->current=current;
  if(current){
    out->fresh=last_reply&&now>=last_reply&&now-last_reply<10000;
#if PET_VARIABLE_OWNERS_ENABLED
    if(owned){out->fresh=owned_fresh_locked(now);if(!out->fresh&&observed)memset(observed,0,sizeof(*observed));}
#endif
    out->legacy=!ui_valid;out->pending=ui_pending;
    out->mode=!out->fresh?7:ui_pending?6:ui_valid?ui_mode:0;
    if(out->fresh){out->count=variable_count;memcpy(out->variables,variables,sizeof(variables));
#if PET_SERVICE_BRIDGE_ENABLED
      for(unsigned i=0;i<out->count;i++)if(out->variables[i].integer){
        int32_t projected;if(pet_bridge_timeline_project(&bridge_timelines,out->variables[i].name,now,&projected)){out->variables[i].number=projected;snprintf(out->variables[i].text,64,"%ld",(long)projected);}
      }
#endif
#if PET_SERVICE_BRIDGE_ENABLED
      out->image_epoch=variable_epoch;
#endif
      out->action_count=ui_count;memcpy(out->actions,ui_actions,sizeof(ui_actions));
#if PET_HOST_COLLECTION_ENABLED
      out->collection=collection_view;
      out->collection_epoch=variable_epoch;
#endif
    }
  }
  if(view&&id&&!strcmp(id,view->widget_id))render_view_locked(view,out->current,out->fresh);
  portEXIT_CRITICAL(&mux);
}
void pet_app_host_render_capture(const char *id,pet_host_render_t *out,pet_p4_miniapp_view_t *view,uint64_t now){
  render_capture(id,out,view,now,NULL,false);
}
#if PET_VARIABLE_OWNERS_ENABLED
void pet_app_host_render_capture_observed(const char *id,pet_host_render_t *out,pet_host_observation_t *observed,uint64_t now){
  render_capture(id,out,NULL,now,observed,true);
}
#endif

/* D outputs use the configured ordered line transport without creating a
 * HOST_VIEW instance or granting a package arbitrary topics. */
bool pet_app_host_emit(const char *topic,const cJSON *payload){
  if(!topic||!payload||!cJSON_IsObject(payload)||(strcmp(topic,"widget/notify")&&strcmp(topic,"widget/data/refresh")))return false;
  pet_p4_send_line_fn send;void *ctx;char board[PET_P4_DEVICE_ID_MAX];
  portENTER_CRITICAL(&mux);send=transport;ctx=transport_ctx;memcpy(board,board_id,sizeof(board));portEXIT_CRITICAL(&mux);
  if(!send||!board[0])return false;
  cJSON *root=cJSON_CreateObject();if(!root)return false;
  cJSON *body=cJSON_Duplicate(payload,true);if(!body){cJSON_Delete(root);return false;}
  if(!cJSON_AddStringToObject(root,"topic",topic)||!cJSON_AddItemToObject(root,"payload",body)){cJSON_Delete(body);cJSON_Delete(root);return false;}
  /* Refresh has a board binding; notify keeps its existing exact payload. */
  if(!strcmp(topic,"widget/data/refresh")&&!cJSON_AddStringToObject(body,"boardDeviceId",board)){cJSON_Delete(root);return false;}
  char *line=cJSON_PrintUnformatted(root);bool ok=line!=NULL;
  if(line)send(line,ctx);
  cJSON_free(line);
  cJSON_Delete(root);
  return ok;
}

#if PET_SERVICE_BRIDGE_ENABLED
void pet_app_host_tick_bound(const char*id,uint32_t rev,uint8_t page,bool visible,uint64_t now,const char*binding){
  if(!id||!id[0]){pet_app_host_tick(id,rev,page,visible,now);return;}
  if(!binding)binding="";
  if(binding[0]){if(strlen(binding)!=64)return;
  for(const char*c=binding;*c;c++)if(!((*c>='0'&&*c<='9')||(*c>='a'&&*c<='f')))return;
  }
  (void)request(id,rev,page,"status",now,visible,NULL,0,NULL,NULL,binding);
}
bool pet_app_host_observed_value_action(const char*id,uint32_t rev,uint8_t page,const char*action,const char*parameter,int32_t value,const pet_host_observation_t*observed,uint64_t now){
  if(!id||!action||strncmp(action,"host.",5)||!pet_bridge_json_name(parameter,31)||!observed||value < -1000000000||value>1000000000)return false;
  cJSON*data=cJSON_CreateObject();if(!data)return false;cJSON_AddNumberToObject(data,parameter,value);
  bool ok=request(id,rev,page,action,now,true,NULL,0,observed,data,NULL);cJSON_Delete(data);return ok;
}
bool pet_app_host_instance_matches(const char*id,const char*token){
  portENTER_CRITICAL(&mux);bool valid=id&&token&&token[0]&&shown&&pet_resources_device_view_current(&resource_view)&&!strcmp(widget,id)&&!strcmp(instance,token);portEXIT_CRITICAL(&mux);return valid;
}
bool pet_app_host_image_ready(const cJSON*p,uint64_t now){
  pet_bridge_image_lease_t lease;bool ready=false;
  if(!pet_bridge_json_name(str(p,"alias"),15))return false;
  portENTER_CRITICAL(&mux);
  bool current=shown&&pet_resources_device_view_current(&resource_view)&&!strcmp(widget,str(p,"componentId"))&&!strcmp(instance,str(p,"instance"))&&!strcmp(ui_context,str(p,"context"))&&last_reply&&now>=last_reply&&now-last_reply<10000;
  if(current)for(unsigned i=0;i<bridge_images.count;i++){
    const pet_bridge_image_ref_t*r=&bridge_images.expected[i];uint32_t w,h;
    if(!strcmp(r->alias,str(p,"alias"))&&!strcmp(r->key,str(p,"key"))&&!strcmp(r->revision,str(p,"revision"))&&pet_bridge_json_uint(p,"width",8,192,&w)&&pet_bridge_json_uint(p,"height",8,192,&h)&&r->width==w&&r->height==h){
      ready=pet_bridge_image_borrow(&bridge_images,r->alias,bridge_pixels,&lease);if(ready)(void)pet_bridge_image_release(&bridge_images,&lease);break;
    }
  }portEXIT_CRITICAL(&mux);return ready;
}
bool pet_app_host_image_apply(const cJSON*p,uint64_t now){
  bool owned __attribute__((cleanup(bridge_prepare_end)))=false;
  portENTER_CRITICAL(&mux);if(!bridge_preparing){bridge_preparing=true;owned=true;}portEXIT_CRITICAL(&mux);
  if(!owned)return false;
  const char*alias=str(p,"alias");pet_bridge_image_ref_t ref={0};
  /* The image packet carries its ref fields at the top level alongside the
   * transport identity. Validate each bounded field without accepting a URL. */
  if(!pet_bridge_json_name(alias,15)||!pet_bridge_json_name(str(p,"key"),47)||!pet_bridge_json_name(str(p,"revision"),64))return false;
  uint32_t w,h,offset;if(!pet_bridge_json_uint(p,"width",8,192,&w)||!pet_bridge_json_uint(p,"height",8,192,&h)||!pet_bridge_json_uint(p,"offset",0,PET_BRIDGE_IMAGE_PIXELS*2,&offset))return false;
  strcpy(ref.alias,alias);strcpy(ref.key,str(p,"key"));strcpy(ref.revision,str(p,"revision"));ref.width=w;ref.height=h;
  const char*data=str(p,"data");size_t length=0;
  if(!data[0]||strlen(data)>2732||mbedtls_base64_decode(bridge_decoded,sizeof(bridge_decoded),&length,(const unsigned char*)data,strlen(data)))return false;
  portENTER_CRITICAL(&mux);
  bool current=shown&&pet_resources_device_view_current(&resource_view)&&!strcmp(widget,str(p,"componentId"))&&!strcmp(instance,str(p,"instance"))&&!strcmp(ui_context,str(p,"context"))&&last_reply&&now>=last_reply&&now-last_reply<10000;
  bool ok=current&&pet_bridge_image_write(&bridge_images,&ref,offset,bridge_decoded,(unsigned)length,bridge_pixels);
  if(ok&&offset+length==w*h*2)variable_epoch++;
  portEXIT_CRITICAL(&mux);return ok;
}
bool pet_app_host_image_borrow(const char*id,const char*alias,uint64_t now,pet_bridge_image_lease_t*out){
  portENTER_CRITICAL(&mux);
  bool ok=id&&shown&&!strcmp(id,widget)&&pet_resources_device_view_current(&resource_view)&&last_reply&&now>=last_reply&&now-last_reply<10000&&pet_bridge_image_borrow(&bridge_images,alias,bridge_pixels,out);
  if(!ok&&out)memset(out,0,sizeof(*out));
  portEXIT_CRITICAL(&mux);
  return ok;
}
void pet_app_host_image_release(pet_bridge_image_lease_t*lease){portENTER_CRITICAL(&mux);(void)pet_bridge_image_release(&bridge_images,lease);portEXIT_CRITICAL(&mux);}
#endif
