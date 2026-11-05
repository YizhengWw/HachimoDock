/* Dedicated audio-only appearance ingress. Included after the shared phase/IO
 * adapters: no legacy begin, raw prepare, slot clear or manifest publication. */
static struct {
  bool active,global;int slot;
  char id[PET_P4_DEVICE_ID_MAX],pack[PET_P4_PACK_ID_MAX];
  pet_p4_send_line_fn sender;void *route;
  uint64_t last_ms;
} g_audio_patch_protocol;
static EXT_RAM_BSS_ATTR uint8_t g_audio_patch_decode[PET_AUDIO_PATCH_CHUNK_BYTES];
static EXT_RAM_BSS_ATTR char g_audio_patch_allowed[PET_P4_ASSET_CATALOG_MAX_FAMILIES][PET_P4_ASSET_FAMILY_MAX];
static unsigned g_audio_patch_allowed_count;
static bool audio_patch_protocol_active(void){return g_audio_patch_protocol.active;}
static bool audio_patch_idle_due(uint64_t now){return g_audio_patch_protocol.active&&g_audio_patch_protocol.last_ms&&now>=g_audio_patch_protocol.last_ms&&now-g_audio_patch_protocol.last_ms>=PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS;}
static bool audio_patch_bound(const pet_p4_runtime_state_t *state,const cJSON *p,int *slot){
  uint32_t n=0;
  if(!state||strcmp(json_string(p,"boardDeviceId"),state->board_device_id)
      ||!pet_p4_boot_identity()[0]||strcmp(json_string(p,"bootId"),pet_p4_boot_identity())
      )return false;
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,"slot");
  if(!cJSON_IsNumber(v)||v->valuedouble<0||v->valuedouble>1||floor(v->valuedouble)!=v->valuedouble)return false;
  n=(uint32_t)v->valuedouble;*slot=(int)n;
  const char *pack=json_string(p,"packId");if(strlen(pack)!=64)return false;
  for(unsigned i=0;i<64;i++)if(!((pack[i]>='0'&&pack[i]<='9')||(pack[i]>='a'&&pack[i]<='f')))return false;
  return slot_is_valid(*slot)&&*slot==pet_p4_asset_active_slot();
}
static bool audio_patch_number(const cJSON *p,const char *key,uint32_t maximum,uint32_t *out){
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);
  if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>maximum||floor(v->valuedouble)!=v->valuedouble)return false;
  *out=(uint32_t)v->valuedouble;return true;
}
static bool audio_patch_families_allowed(const cJSON *files,bool global){
  if(!cJSON_IsArray(files)||cJSON_GetArraySize(files)<0||cJSON_GetArraySize(files)>(int)PET_AUDIO_PATCH_MAX_FILES)return false;
  for(int i=0;i<cJSON_GetArraySize(files);i++){
    const char *family=json_string(cJSON_GetArrayItem(files,i),"family");bool allowed=pet_p4_system_cue_family(family);
    if(global)allowed=pet_audio_patch_global_family_known(family);
    for(unsigned j=0;!global&&!allowed&&j<g_audio_patch_allowed_count;j++)allowed=!strcmp(family,g_audio_patch_allowed[j]);
    if(!allowed)return false;
  }
  return true;
}
static void audio_patch_reply(const pet_appearance_commit_io_t *io,pet_p4_send_line_fn send,void *route,
    const char *id,const char *phase,const char *path,int slot,const char *pack,const char *board,const char *boot,
    pet_appearance_commit_result_t result,uint32_t index,uint32_t received,uint32_t generation){
  /* Caller keeps this owner pinned while borrowing the state mutex for JSON/TX. */
  if(!io->begin(io->context))return;
  cJSON *p=cJSON_CreateObject();bool ok=p&&cJSON_AddStringToObject(p,"transferId",id)
    &&cJSON_AddStringToObject(p,"phase",phase)&&cJSON_AddBoolToObject(p,"ok",result==PET_APPEARANCE_COMMIT_OK)
    &&cJSON_AddStringToObject(p,"boardDeviceId",board)&&cJSON_AddStringToObject(p,"bootId",boot)
    &&cJSON_AddNumberToObject(p,"slot",slot)&&cJSON_AddStringToObject(p,"packId",pack);
  if(ok&&path[0])ok=cJSON_AddStringToObject(p,"path",path)&&cJSON_AddNumberToObject(p,"index",index);
  if(ok&&path[0]&&result==PET_APPEARANCE_COMMIT_OK)ok=cJSON_AddNumberToObject(p,"nextIndex",index+1U)&&cJSON_AddNumberToObject(p,"receivedBytes",received);
  if(ok&&generation&&result==PET_APPEARANCE_COMMIT_OK)ok=cJSON_AddNumberToObject(p,"audioGeneration",generation)
    &&cJSON_AddNumberToObject(p,"files",pet_audio_patch_file_count())
    &&cJSON_AddNumberToObject(p,"totalBytes",pet_audio_patch_total_bytes());
  if(ok&&result!=PET_APPEARANCE_COMMIT_OK)ok=cJSON_AddStringToObject(p,"errorCode",result==PET_APPEARANCE_COMMIT_UNKNOWN?"audio_patch_outcome_unknown":"audio_patch_rejected")
    &&cJSON_AddStringToObject(p,"error",result==PET_APPEARANCE_COMMIT_UNKNOWN?"audio patch outcome unknown; do not replay":"audio patch rejected; original appearance preserved");
  if(ok&&asset_phase_send_current(send,route)){send_topic_payload(send,route,"asset/ack",p);p=NULL;}
  cJSON_Delete(p);io->end(io->context);
}
static void audio_patch_retire(pet_p4_runtime_state_t *state){
  (void)pet_p4_appearance_end(g_audio_patch_protocol.sender,g_audio_patch_protocol.route,g_audio_patch_protocol.id,PET_APPEARANCE_SERIAL);
  memset(&g_audio_patch_protocol,0,sizeof(g_audio_patch_protocol));pet_audio_patch_retire();state->asset_transfer_active=false;
}
static void handle_audio_patch_topic(pet_p4_runtime_state_t *state,const char *topic,const cJSON *p,pet_p4_send_line_fn send,void *route){
  const char *id=json_string(p,"transferId"),*path=json_string(p,"path"),*pack=json_string(p,"packId"),*phase=topic+6;
  const bool begin=!strcmp(topic,"asset/audio-begin"),chunk=!strcmp(topic,"asset/audio-chunk"),commit=!strcmp(topic,"asset/audio-commit"),abort=!strcmp(topic,"asset/audio-abort");
  const pet_appearance_commit_io_t io={g_asset_query_io_begin,g_asset_query_io_end,NULL,g_asset_query_io_context};
  bool global=!strcmp(json_string(p,"scope"),"global");
  int slot=-1;uint32_t index=0,offset=0,total=0;char board[PET_P4_DEVICE_ID_MAX],boot[17];
  copy_text(board,sizeof(board),state?state->board_device_id:"");copy_text(boot,sizeof(boot),pet_p4_boot_identity());
  bool admitted=state&&io.begin&&io.end&&asset_phase_send_current(send,route)&&pet_appearance_id_valid(id)
    &&(!json_string(p,"scope")[0]||global)
    &&(begin||chunk||commit||abort)&&audio_patch_bound(state,p,&slot)&&!pet_p4_appearance_mutation_blocked();
  if(begin)admitted=admitted&&!pet_p4_appearance_active()&&!g_asset_chunk_file&&!g_raw_asset_chunk.active&&g_asset_transfer_slot<0
    &&!pet_p4_diagnostics_reboot_pending()&&!pet_p4_ota_transfer_active()
    &&audio_patch_number(p,"totalBytes",PET_AUDIO_PATCH_MAX_BYTES,&total);
  else admitted=admitted&&g_audio_patch_protocol.global==global&&g_audio_patch_protocol.active&&g_audio_patch_protocol.slot==slot
    &&!strcmp(g_audio_patch_protocol.pack,pack)&&pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL)
    &&g_audio_patch_protocol.sender==send&&g_audio_patch_protocol.route==route&&!strcmp(g_audio_patch_protocol.id,id);
  if(chunk)admitted=admitted&&audio_patch_number(p,"index",UINT32_MAX-1U,&index)&&audio_patch_number(p,"offset",PET_AUDIO_PATCH_MAX_FILE,&offset);
  if(!admitted){
    if(io.begin&&io.end)audio_patch_reply(&io,send,route,id,phase,path,slot,pack,board,boot,PET_APPEARANCE_COMMIT_FAILED,index,0,0);
    return;
  }
  if(begin){
    if(!pet_p4_appearance_claim(send,route,id,PET_APPEARANCE_SERIAL)){
      audio_patch_reply(&io,send,route,id,phase,path,slot,pack,board,boot,PET_APPEARANCE_COMMIT_FAILED,0,0,0);return;
    }
    g_audio_patch_protocol.active=true;g_audio_patch_protocol.global=global;g_audio_patch_protocol.slot=slot;g_audio_patch_protocol.sender=send;g_audio_patch_protocol.route=route;
    copy_text(g_audio_patch_protocol.id,sizeof(g_audio_patch_protocol.id),id);copy_text(g_audio_patch_protocol.pack,sizeof(g_audio_patch_protocol.pack),pack);
    g_audio_patch_allowed_count=state->asset_catalog.count<PET_P4_ASSET_CATALOG_MAX_FAMILIES?state->asset_catalog.count:PET_P4_ASSET_CATALOG_MAX_FAMILIES;
    for(unsigned i=0;i<g_audio_patch_allowed_count;i++)copy_text(g_audio_patch_allowed[i],sizeof(g_audio_patch_allowed[i]),state->asset_catalog.entries[i].family);
  }
  state->asset_transfer_active=true;
  pet_appearance_phase_t operation={0};
  if(!pet_p4_appearance_phase_begin(&operation,state,send,route,id,PET_APPEARANCE_SERIAL,slot,&io)){
    if(begin)audio_patch_retire(state);
    audio_patch_reply(&io,send,route,id,phase,path,slot,pack,board,boot,PET_APPEARANCE_COMMIT_FAILED,index,0,0);return;
  }
  /* State lock is borrowed here; decode/FS/metadata work owns STORAGE_WORK. */
  bool attempted=false,ok=false;uint32_t generation=0,received=0,next_index=0;char actual_pack[65];
  bool same=read_ready_slot_pack_id(slot,actual_pack,sizeof(actual_pack))&&!strcmp(actual_pack,pack);
  if(same&&begin)ok=audio_patch_families_allowed(cJSON_GetObjectItemCaseSensitive(p,"files"),global)
    &&pet_audio_patch_begin(global?PET_AUDIO_PATCH_GLOBAL_SLOT:slot,global?PET_AUDIO_PATCH_GLOBAL_PACK:pack,cJSON_GetObjectItemCaseSensitive(p,"files"),total,&attempted);
  else if(same&&chunk){
    const cJSON *data=cJSON_GetObjectItemCaseSensitive(p,"data");size_t decoded=0;
    ok=cJSON_IsString(data)&&strlen(data->valuestring)>0&&strlen(data->valuestring)<=5464U
      &&mbedtls_base64_decode(g_audio_patch_decode,sizeof(g_audio_patch_decode),&decoded,(const unsigned char*)data->valuestring,strlen(data->valuestring))==0
      &&pet_audio_patch_chunk(path,index,offset,g_audio_patch_decode,decoded,&attempted);
    if(ok)ok=pet_audio_patch_progress(path,&next_index,&received)&&next_index==index+1U;
  }else if(same&&commit)ok=pet_audio_patch_commit(&attempted,&generation);
  else if(same&&abort)ok=pet_audio_patch_abort(&attempted);
  pet_appearance_commit_result_t result=pet_p4_appearance_phase_end(&operation,state,slot,ok,attempted);
  if(result==PET_APPEARANCE_COMMIT_OK)g_audio_patch_protocol.last_ms=(uint64_t)esp_timer_get_time()/1000ULL;
  uint64_t reply_generation=g_appearance_session.generation;
  bool pinned=pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL);
  if(pinned)g_appearance_session.operation_active=true;
  audio_patch_reply(&io,send,route,id,phase,path,slot,pack,board,boot,result,index,received,generation);
  if(pinned&&g_appearance_session.generation==reply_generation&&pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL))g_appearance_session.operation_active=false;
  if(result==PET_APPEARANCE_COMMIT_OK&&(commit||abort))audio_patch_retire(state);
  else if(result==PET_APPEARANCE_COMMIT_FAILED&&begin)audio_patch_retire(state);
}
static void audio_patch_timeout_worker(pet_p4_runtime_state_t *state){
  uint64_t now=(uint64_t)esp_timer_get_time()/1000ULL;
  if(!state||!audio_patch_idle_due(now)||pet_p4_appearance_mutation_blocked())return;
  const pet_appearance_commit_io_t io={g_asset_query_io_begin,g_asset_query_io_end,NULL,g_asset_query_io_context};
  int slot=g_audio_patch_protocol.slot;pet_appearance_phase_t operation={0};
  if(!pet_p4_appearance_phase_begin(&operation,state,g_audio_patch_protocol.sender,g_audio_patch_protocol.route,g_audio_patch_protocol.id,PET_APPEARANCE_SERIAL,slot,&io))return;
  bool attempted=false,ok=pet_audio_patch_abort(&attempted);
  pet_appearance_commit_result_t result=pet_p4_appearance_phase_end(&operation,state,slot,ok,attempted);
  if(result==PET_APPEARANCE_COMMIT_OK)audio_patch_retire(state);
}
bool pet_p4_asset_audio_override_path(const char *family,char *out,size_t capacity){
  /* Playback already owns its realtime reservation; no state mutex or renderer
   * call here. A STORAGE_WORK mutation cannot overlap this physical read. */
  return pet_audio_patch_resolve(PET_AUDIO_PATCH_GLOBAL_SLOT,PET_AUDIO_PATCH_GLOBAL_PACK,family,out,capacity);
}
