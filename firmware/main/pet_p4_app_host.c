/* Bounded request/reply transport. Never retransmit an action on timeout. */
#include "pet_p4_app_host.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static pet_p4_send_line_fn transport;
static void *transport_ctx;
static char board_id[PET_P4_DEVICE_ID_MAX];
static char widget[48],instance[32],values[8][160];
static uint32_t generation,sequence,applied;
static uint64_t last_poll,last_reply,last_action;
static bool shown;
static bool ui_valid,ui_pending;
static uint8_t ui_mode,ui_count;
static char ui_actions[16][48],ui_context[65];
static const char *names[]={"eyebrow","headline","metricValue","metricLabel","metricUnit","badge","note","footer"};
static const unsigned sizes[]={96,160,64,96,32,16,160,160};
static const char *str(const cJSON *p,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);return cJSON_IsString(v)?v->valuestring:"";}
void pet_app_host_transport(pet_p4_send_line_fn send,void *ctx,const char *board){portENTER_CRITICAL(&mux);transport=send;transport_ctx=ctx;snprintf(board_id,sizeof(board_id),"%s",board?board:"");portEXIT_CRITICAL(&mux);}
static bool request(const char *id,uint32_t rev,uint8_t page,const char *action,uint64_t now,bool visible,const char *confirmed_context){
  char token[32],board[PET_P4_DEVICE_ID_MAX];uint32_t seq;pet_p4_send_line_fn send;void *ctx;
  uint32_t nonce=esp_random();
  portENTER_CRITICAL(&mux);
  if(strcmp(widget,id)||generation!=rev){
    snprintf(widget,sizeof(widget),"%s",id);generation=rev;
    snprintf(instance,sizeof(instance),"%08lx-%08lx",(unsigned long)nonce,(unsigned long)rev);
    sequence=applied=0;last_poll=last_reply=last_action=0;memset(values,0,sizeof(values));ui_valid=ui_pending=false;ui_count=0;ui_context[0]=0;
  }
  if(!transport||(!strcmp(action,"status")&&now-last_poll<2000&&shown==visible)||(strcmp(action,"status")&&last_action&&now-last_action<150)){
    portEXIT_CRITICAL(&mux);return false;
  }
  bool command=strcmp(action,"status")!=0;
  if(command)last_action=now;
  shown=visible;last_poll=now;seq=++sequence;
  if(command){applied=seq;last_reply=now;ui_pending=true;snprintf(values[1],sizeof(values[1]),"正在处理");snprintf(values[6],sizeof(values[6]),"已请求电脑，不会重复派发");}
  memcpy(token,instance,sizeof(token));memcpy(board,board_id,sizeof(board));send=transport;ctx=transport_ctx;
  portEXIT_CRITICAL(&mux);
  cJSON *root=cJSON_CreateObject();if(!root)return false;
  cJSON_AddStringToObject(root,"topic","app/event");cJSON *p=cJSON_AddObjectToObject(root,"payload");
  if(p){cJSON_AddStringToObject(p,"boardDeviceId",board);cJSON_AddStringToObject(p,"componentId",id);cJSON_AddStringToObject(p,"instance",token);cJSON_AddNumberToObject(p,"sequence",seq);
    cJSON_AddStringToObject(p,"action",action);cJSON_AddNumberToObject(p,"page",page);cJSON_AddBoolToObject(p,"visible",visible);
    if(confirmed_context){cJSON_AddBoolToObject(p,"confirmed",true);cJSON_AddStringToObject(p,"context",confirmed_context);}}
  char *line=p?cJSON_PrintUnformatted(root):NULL;if(line)send(line,ctx);bool ok=line!=NULL;cJSON_free(line);cJSON_Delete(root);return ok;
}
void pet_app_host_tick(const char *id,uint32_t rev,uint8_t page,bool visible,uint64_t now){if(id&&id[0])request(id,rev,page,"status",now,visible,NULL);}
bool pet_app_host_action(const char *id,uint32_t rev,uint8_t page,const char *action,uint64_t now){
  if(!id||!id[0]||!action||strncmp(action,"host.",5)||strlen(action)>47)return false;
  if(!pet_app_host_enabled(id,action,now))return true;
  (void)request(id,rev,page,action,now,true,NULL);
  return true; /* A throttled host action must not fall through to desktop input. */
}
bool pet_app_host_confirmed_action(const char *id,uint32_t rev,uint8_t page,const char *action,const char *context,uint64_t now){
  if(!id||!action||strncmp(action,"host.",5)||strlen(action)>47||!context||strlen(context)>64)return false;
  if(!pet_app_host_enabled(id,action,now))return true;
  (void)request(id,rev,page,action,now,true,context);return true;
}
int pet_app_host_mode(const char *id,uint64_t now){
  portENTER_CRITICAL(&mux);int mode=strcmp(id,widget)||!last_reply||now-last_reply>=10000?7:ui_pending?6:ui_valid?ui_mode:0;portEXIT_CRITICAL(&mux);return mode;
}
bool pet_app_host_enabled(const char *id,const char *action,uint64_t now){
  portENTER_CRITICAL(&mux);
  bool ok=!ui_valid; // Legacy hosts retain their earlier behavior.
  if(!strcmp(id,widget)&&ui_valid){ok=false;if(!ui_pending&&last_reply&&now-last_reply<10000)for(int i=0;i<ui_count;i++)if(!strcmp(action,ui_actions[i]))ok=true;}
  portEXIT_CRITICAL(&mux);return ok;
}
void pet_app_host_context(const char *id,char context[65]){
  portENTER_CRITICAL(&mux);snprintf(context,65,"%s",!strcmp(id,widget)?ui_context:"");portEXIT_CRITICAL(&mux);
}
bool pet_app_host_apply(const cJSON *p,uint64_t now){
  const cJSON *s=cJSON_GetObjectItemCaseSensitive(p,"sequence"),*v=cJSON_GetObjectItemCaseSensitive(p,"values");
  if(!cJSON_IsNumber(s)||!isfinite(s->valuedouble)||s->valuedouble<1||s->valuedouble>UINT32_MAX||floor(s->valuedouble)!=s->valuedouble||!cJSON_IsObject(v))return false;
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
  portENTER_CRITICAL(&mux);
  bool ok=!strcmp(str(p,"componentId"),widget)&&!strcmp(str(p,"instance"),instance)&&(uint32_t)s->valuedouble<=sequence&&(uint32_t)s->valuedouble>=applied;
  if(ok){for(int i=0;i<8;i++)snprintf(values[i],sizeof(values[i]),"%s",str(v,names[i]));applied=(uint32_t)s->valuedouble;last_reply=now;ui_valid=ui!=NULL;ui_pending=false;ui_mode=mode;ui_count=count;memcpy(ui_actions,actions,sizeof(actions));memcpy(ui_context,context,sizeof(context));}
  portEXIT_CRITICAL(&mux);return ok;
}
void pet_app_host_view(pet_p4_miniapp_view_t *out,uint64_t now){
  char *dest[]={out->eyebrow,out->headline,out->metric_value,out->metric_label,out->metric_unit,out->badge,out->note,out->footer};
  portENTER_CRITICAL(&mux);
  if(!strcmp(out->widget_id,widget)){
    if(last_reply&&now-last_reply<10000){for(int i=0;i<8;i++)snprintf(dest[i],sizes[i],"%s",values[i]);out->revision^=applied;}
    else {snprintf(out->headline,sizeof(out->headline),"等待电脑连接");out->metric_value[0]=0;snprintf(out->note,sizeof(out->note),"请保持 Pet Manager 运行并启用此组件");}
  }
  portEXIT_CRITICAL(&mux);
}
