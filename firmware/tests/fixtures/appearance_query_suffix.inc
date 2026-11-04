
static const char *str(const char *key){return json_string(captured,key);}
static void expect_bool(const char*key,bool value){const cJSON*v=cJSON_GetObjectItemCaseSensitive(captured,key);assert(cJSON_IsBool(v)&&cJSON_IsTrue(v)==value);}
static void expect_not_proof(bool unknown,const char *id,const char *nonce){
 assert(captured&&cJSON_GetArraySize(captured)==11);
 assert(!strcmp(str("transferId"),id)&&!strcmp(str("queryNonce"),nonce));
 assert(!strcmp(str("boardDeviceId"),"synthetic-board")&&!strcmp(str("bootId"),"0000000000000001"));
 assert(!strcmp(str("observationSchema"),APPEARANCE_OBSERVATION_SCHEMA));
 assert(!strcmp(str("errorCode"),unknown?"appearance_query_outcome_unknown":"appearance_query_busy"));
 expect_bool("ok",false);expect_bool("proof",false);expect_bool("quiescent",false);expect_bool("serialized",true);
 assert(!cJSON_GetObjectItemCaseSensitive(captured,"slots")&&!cJSON_GetObjectItemCaseSensitive(captured,"activeSlot")&&!cJSON_GetObjectItemCaseSensitive(captured,"nativeIoEpoch"));
}
int main(int argc,char**argv){
 assert(argc==2);cJSON_Hooks hooks={tracked_alloc,tracked_free};cJSON_InitHooks(&hooks);const char*which=argv[1];strcpy(state.board_device_id,"synthetic-board");state.asset_revision=3;
 private_tracking=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);assert(private_tracking!=MAP_FAILED);
 g_asset_query_io_begin=io_begin;g_asset_query_io_end=io_end;g_asset_query_pending=pending;
 cJSON*q=cJSON_Parse("{\"observationSchema\":\"p4-appearance-quiescent-v1\",\"queryNonce\":\"observe-synthetic\",\"transferId\":\"observe-synthetic\",\"expectedBoardDeviceId\":\"synthetic-board\",\"expectedBootId\":\"0000000000000001\"}");assert(q);
 const char*id="observe-synthetic";const char*nonce=id;bool invalid=false,legacy=false,idle=false;
 g_appearance_session.operation_active=true;
 if(!strcmp(which,"owner")){g_appearance_session.operation_active=false;g_appearance_session.active=true;}
 else if(!strcmp(which,"transfer")){g_appearance_session.operation_active=false;state.asset_transfer_active=true;}
 else if(!strcmp(which,"unknown")){g_appearance_session.operation_active=false;g_appearance_session.recovery_blocked=true;}
 else if(!strcmp(which,"unknown-operation")){g_appearance_session.recovery_blocked=true;}
 else if(!strcmp(which,"storage")){g_appearance_session.operation_active=false;forbid_storage=false;storage_busy=true;}
 else if(!strcmp(which,"storage-unavailable")){g_appearance_session.operation_active=false;forbid_storage=false;storage_unavailable=true;invalid=true;}
 else if(!strcmp(which,"distinct-tokens")){id="slots-synthetic";cJSON_ReplaceItemInObjectCaseSensitive(q,"transferId",cJSON_CreateString(id));}
 else if(!strcmp(which,"identity-snapshot"))change_identity=true;
 else if(!strcmp(which,"callback-refusal"))refuse_callback=true;
 else if(!strncmp(which,"oom-",4))allocation_budget=atoi(which+4);
 else if(!strcmp(which,"bad-transfer")){id="bad id";cJSON_ReplaceItemInObjectCaseSensitive(q,"transferId",cJSON_CreateString(id));invalid=true;}
 else if(!strcmp(which,"missing-transfer")){cJSON_DeleteItemFromObjectCaseSensitive(q,"transferId");invalid=true;}
 else if(!strncmp(which,"duplicate-",10)){
  const char*key=!strcmp(which+10,"board")?"expectedBoardDeviceId":!strcmp(which+10,"boot")?"expectedBootId":!strcmp(which+10,"schema")?"observationSchema":"transferId";
  cJSON_AddStringToObject(q,key,json_string(q,key));invalid=true;
 }
 else if(!strcmp(which,"board")){cJSON_ReplaceItemInObjectCaseSensitive(q,"expectedBoardDeviceId",cJSON_CreateString("other-board"));invalid=true;}
 else if(!strcmp(which,"boot")){cJSON_ReplaceItemInObjectCaseSensitive(q,"expectedBootId",cJSON_CreateString("old-boot"));invalid=true;}
 else if(!strcmp(which,"nonce")){cJSON_ReplaceItemInObjectCaseSensitive(q,"queryNonce",cJSON_CreateString("bad nonce"));invalid=true;}
 else if(!strcmp(which,"id")){id="different-token";invalid=true;}
 else if(!strcmp(which,"duplicate")){cJSON_AddStringToObject(q,"queryNonce",nonce);invalid=true;}
 else if(!strcmp(which,"partial")){cJSON_DeleteItemFromObjectCaseSensitive(q,"observationSchema");invalid=true;}
 else if(!strcmp(which,"schema")){cJSON_ReplaceItemInObjectCaseSensitive(q,"observationSchema",cJSON_CreateString("unknown-schema"));invalid=true;}
 else if(!strcmp(which,"legacy")){cJSON_Delete(q);q=cJSON_Parse("{\"transferId\":\"observe-synthetic\"}");legacy=true;}
 else if(!strcmp(which,"idle")){
  g_appearance_session.operation_active=false;forbid_storage=false;idle=true;
  assert(!mprotect(private_tracking,4096,PROT_READ|PROT_WRITE));private_tracking->active=false;
  for(int i=0;i<2;i++){char path[32];snprintf(path,sizeof(path),"slot-%d.synthetic",i);FILE*f=fopen(path,"wb");assert(f);for(int j=0;j<64;j++)assert(fputc('a'+i,f)!=EOF);assert(!fclose(f));}
 }
 pet_appearance_session_t before=g_appearance_session;
 send_asset_slot_query_ack(&state,q,capture,&state,id);
 assert(held&&!memcmp(&before,&g_appearance_session,sizeof(before)));
 if(!idle){assert(!disk_reads&&!snapshots);if(!storage_busy&&!storage_unavailable)assert(!broker_calls);}
 else {assert(disk_reads==2&&snapshots==2&&broker_calls==2);expect_bool("ok",true);expect_bool("quiescent",true);assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(captured,"slots"))==2);}
 if(refuse_callback||!strncmp(which,"oom-",4)){assert(!sends&&!captured);}
 else if(invalid||legacy){assert(sends==1);expect_bool("ok",false);assert(!cJSON_GetObjectItemCaseSensitive(captured,"observationSchema")&&!cJSON_GetObjectItemCaseSensitive(captured,"errorCode"));}
 else if(!idle)expect_not_proof(g_appearance_session.recovery_blocked,id,nonce);
 if(captured){char*out=cJSON_PrintUnformatted(captured);assert(out);puts(out);cJSON_free(out);}
 allocation_budget=-1;cJSON_Delete(captured);cJSON_Delete(q);assert(!allocations);assert(!munmap(private_tracking,4096));return 0;
}
