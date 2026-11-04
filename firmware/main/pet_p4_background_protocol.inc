/* Explicit control of verified installed pure programs. No code, credential,
 * native pointer, execution budget or arbitrary effect input on this channel. */
static bool background_hex(const cJSON *p,const char *key,uint32_t *out){
  const char *s=json_string(p,key);if(strlen(s)!=8)return false;uint32_t n=0;
  for(unsigned i=0;i<8;i++){unsigned char c=(unsigned char)s[i];unsigned v;
    if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else if(c>='A'&&c<='F')v=c-'A'+10;else return false;
    n=(n<<4)|v;
  }*out=n;return true;
}
static bool background_serial(const cJSON *p,uint64_t *out){
  const char *s=json_string(p,"registration");size_t n=strlen(s);if(!n||n>20||s[0]<'1'||s[0]>'9')return false;
  uint64_t value=0;
  for(size_t i=0;i<n;i++){if(s[i]<'0'||s[i]>'9')return false;unsigned digit=(unsigned)(s[i]-'0');if(value>(UINT64_MAX-digit)/10)return false;value=value*10+digit;}
  *out=value;return true;
}
static bool background_fields(const cJSON *p,bool start,bool stop,bool inspect){
  static const char *const a[]={"requestId","widgetId","widgetChecksum","buttonsChecksum","spritesChecksum","bootId"};
  static const char *const b[]={"requestId","widgetId","registration","bootId"};
  static const char *const q[]={"requestId"};
  static const char *const c[]={"requestId","widgetId"};
  const char *const *keys=start?a:stop?b:inspect?c:q;unsigned count=start?6:stop?4:inspect?2:1;
  if(!cJSON_IsObject(p)||cJSON_GetArraySize(p)!=(int)count)return false;
  for(unsigned i=0;i<count;i++){
    unsigned seen=0;const cJSON *v;cJSON_ArrayForEach(v,p)if(v->string&&!strcmp(v->string,keys[i]))seen++;
    if(seen!=1)return false;
  }
  const char *rid=json_string(p,"requestId");if(!rid[0]||strlen(rid)>64)return false;
  for(const unsigned char *s=(const unsigned char*)rid;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'||*s=='.'))return false;
  return true;
}
static bool background_boot_current(const cJSON *p){
  const char *requested=json_string(p,"bootId"),*current=pet_p4_boot_identity();
  if(!current||strlen(current)!=16||strlen(requested)!=16)return false;
  for(unsigned i=0;i<16;i++)if(!((requested[i]>='0'&&requested[i]<='9')||(requested[i]>='a'&&requested[i]<='f')))return false;
  return !strcmp(requested,current);
}
static bool handle_background_program_topic(pet_p4_runtime_state_t *state,const char *topic,const cJSON *p,pet_p4_send_line_fn send,void *ctx){
  bool query=!strcmp(topic,"widget/program-query"),start=!strcmp(topic,"widget/program-start"),stop=!strcmp(topic,"widget/program-stop");
  bool inspect=!strcmp(topic,"widget/program-inspect");
  char error[160]={0};bool ok=(query||start||stop||inspect)&&background_fields(p,start,stop,inspect);
  uint32_t content[3]={0};uint64_t registration=0;
  if(ok&&start)ok=background_hex(p,"widgetChecksum",&content[0])&&background_hex(p,"buttonsChecksum",&content[1])&&background_hex(p,"spritesChecksum",&content[2]);
  if(ok&&stop)ok=background_serial(p,&registration);
  if(!ok)copy_text(error,sizeof(error),"invalid background control fields");
  else if((start||stop)&&!background_boot_current(p)){ok=false;copy_text(error,sizeof(error),"background boot changed; request not applied");}
  else if(!pet_p4_miniapp_background_available()){ok=false;copy_text(error,sizeof(error),"background capability unavailable");}
  else if(start)ok=pet_p4_miniapp_background_start(json_string(p,"widgetId"),content,&registration,error,sizeof(error));
  else if(stop)ok=pet_p4_miniapp_background_stop(json_string(p,"widgetId"),registration,error,sizeof(error));
  else if(inspect){ok=pet_p4_miniapp_background_content(json_string(p,"widgetId"),content);if(!ok)copy_text(error,sizeof(error),"verified installed content unavailable");}
  cJSON *out=cJSON_CreateObject(),*items=cJSON_CreateArray();
  if(!out||!items){cJSON_Delete(out);cJSON_Delete(items);return false;}
  bool complete=cJSON_AddBoolToObject(out,"ok",ok)
    &&cJSON_AddStringToObject(out,"requestId",background_fields(p,start,stop,inspect)?json_string(p,"requestId"):"")
    &&cJSON_AddStringToObject(out,"boardDeviceId",state?state->board_device_id:"")
    &&cJSON_AddStringToObject(out,"engine",PET_P4_MINIAPP_BACKGROUND_ENGINE)
    &&cJSON_AddStringToObject(out,"bootId",pet_p4_boot_identity());
  if(!complete||(error[0]&&!cJSON_AddStringToObject(out,"error",error))){cJSON_Delete(out);cJSON_Delete(items);return false;}
  if(inspect&&ok){
    cJSON *identity=cJSON_CreateObject();char checksums[3][9];
    if(!identity){cJSON_Delete(out);cJSON_Delete(items);return false;}
    for(unsigned i=0;i<3;i++)snprintf(checksums[i],sizeof(checksums[i]),"%08x",content[i]);
    complete=cJSON_AddStringToObject(identity,"widgetId",json_string(p,"widgetId"))
      &&cJSON_AddStringToObject(identity,"widgetChecksum",checksums[0])
      &&cJSON_AddStringToObject(identity,"buttonsChecksum",checksums[1])
      &&cJSON_AddStringToObject(identity,"spritesChecksum",checksums[2]);
    if(!complete||!cJSON_AddItemToObject(out,"installedContent",identity)){cJSON_Delete(identity);cJSON_Delete(out);cJSON_Delete(items);return false;}
  }
  pet_p4_miniapp_background_status_t status[PET_P4_MINIAPP_BACKGROUND_MAX];
  size_t entries=pet_p4_miniapp_background_list(status);
  if(entries>PET_P4_MINIAPP_BACKGROUND_MAX){cJSON_Delete(out);cJSON_Delete(items);return false;}
  for(size_t i=0;i<entries;i++){
    const pet_p4_miniapp_background_status_t *s=&status[i];cJSON *item=cJSON_CreateObject();
    if(!item){cJSON_Delete(out);cJSON_Delete(items);return false;}
    char serial[24];snprintf(serial,sizeof(serial),"%llu",(unsigned long long)s->registration);
    complete=cJSON_AddStringToObject(item,"widgetId",s->widget_id)&&cJSON_AddStringToObject(item,"registration",serial)
      &&cJSON_AddBoolToObject(item,"live",s->live)&&cJSON_AddBoolToObject(item,"stopped",s->stopped)
      &&cJSON_AddBoolToObject(item,"checkpointPending",s->checkpoint_pending)&&cJSON_AddNumberToObject(item,"revision",s->revision)
      &&cJSON_AddNumberToObject(item,"phase",s->phase)&&cJSON_AddNumberToObject(item,"lastStepUs",s->last_step_us)&&cJSON_AddNumberToObject(item,"maxStepUs",s->max_step_us);
    if(!complete||!cJSON_AddItemToArray(items,item)){cJSON_Delete(item);cJSON_Delete(out);cJSON_Delete(items);return false;}
  }
  /* Never publish a partial/empty success if allocation failed after a real
   * mutation. Missing receipt stays unknown; the client must not replay it. */
  if(!cJSON_AddItemToObject(out,"items",items)){cJSON_Delete(out);cJSON_Delete(items);return false;}
  send_topic_payload(send,ctx,"widget/program-status",out);return ok;
}
