static unsigned long long native_hash_file(FILE*f,unsigned long long*n){return fnv1a64_file(f,n);}
static void handle_json_frame(const uint8_t*p,uint32_t n){assert(handle_asset_abort_json(p,n));}
static void interleave(void){
  if(interleaving)return;interleaving=true;assert(!held);frames++;
  void*saved=task;task=(void*)1;held=true;probe=true;
  uint64_t generation=g_appearance_session.generation;
  if(g_appearance_session.operation_active){
    if(header_preempt){
      header_preempt=false;
      /* Actual broker revocation, not a fake current() result. */
      assert(pet_resources_realtime_begin(&broker,&header_realtime)==PET_RESOURCE_OK);
    }
    assert(!pet_p4_appearance_end(g_appearance_session.sender,(void*)g_appearance_session.route,g_appearance_session.id,g_appearance_session.kind));
    assert(!pet_p4_appearance_claim(sender,(void*)99,"steal",PET_APPEARANCE_SERIAL));
    unsigned before=io_calls;
    cJSON*q=cJSON_Parse("{\"transferId\":\"probe\"}");send_asset_slot_query_ack(&state,q,sender,NULL,"probe");cJSON_Delete(q);
    assert(io_calls==before); /* busy query must neither stat nor inspect tracking */
    pet_p4_asset_transfer_process(&state,90000);pet_p4_native_usb_process(90000);
    assert(io_calls==before&&generation==g_appearance_session.generation);
    pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&owner)==PET_RESOURCE_OK);
    pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);
    pet_resource_result_t admitted=pet_resources_device_acquire(owner,PET_RESOURCE_STORAGE_WORK,1,&lease);
    if(usage.leases[PET_RESOURCE_STORAGE_WORK]||(usage.protected_mask&PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK)))assert(admitted!=PET_RESOURCE_OK);
    else{
      /* A typed idle ACK owns metadata only, not a physical storage lease.
       * Existing operation_active still blocked release/claim above. */
      assert(!g_raw_asset_chunk.active&&admitted==PET_RESOURCE_OK);
      assert(pet_resources_device_finish(lease)==PET_RESOURCE_FINISH_CURRENT);
    }
    pet_resources_device_revoke(owner);
  }
  held=false;task=saved;probe=false;
  if(mode==1&&!quarantine_injected&&write_calls){atomic_fetch_add(&g_connection_generation,1);quarantine_injected=true;}
  if(mode==2&&!quarantine_injected&&write_calls){state.asset_revision++;quarantine_injected=true;}
  interleaving=false;
}
static cJSON*body(const char*s){cJSON*r=cJSON_Parse(s);assert(r);return r;}
static void serial(const char*topic,const char*s){cJSON*p=body(s);handle_asset_topic(&state,topic,p,sender,NULL);cJSON_Delete(p);assert(held);}
static void native(unsigned kind,const char*s){handle_complete_frame(kind,(const uint8_t*)s,(uint32_t)strlen(s));assert(held);}
static void reset_counts(void){acks=negative_acks=positive_acks=0;}
static void begin_serial(void){serial("asset/begin","{\"transferId\":\"tx\",\"totalBytes\":131072,\"rawBytes\":131072}");assert(acks==1&&positive_acks==1&&state.asset_transfer_active);reset_counts();}
static void assert_quarantine(void){
  assert(g_appearance_session.active&&g_appearance_session.recovery_blocked);
  unsigned before=erase_calls+write_calls+rename_calls;
  probe=true;serial("asset/abort","{\"transferId\":\"tx\"}");probe=false;
  pet_p4_asset_transfer_process(&state,90000);pet_p4_native_usb_process(90000);
  assert(before==erase_calls+write_calls+rename_calls);
}
int main(int argc,char**argv){
  assert(argc==2);cJSON_Hooks hooks={json_malloc,json_free};cJSON_InitHooks(&hooks);const char*c=argv[1];strcpy(state.board_device_id,"synthetic-board");
  if(!strcmp(c,"cue-owner-lifetime")){
    assert(!cue_deferred);assert(pet_p4_appearance_claim(sender,NULL,"cue",PET_APPEARANCE_NATIVE));
    assert(cue_deferred);pet_p4_asset_transfer_process(&state,1);assert(cue_deferred);
    assert(!pet_p4_appearance_claim(sender,NULL,"foreign",PET_APPEARANCE_SERIAL));assert(cue_deferred);
    assert(pet_p4_appearance_end(sender,NULL,"cue",PET_APPEARANCE_NATIVE));assert(cue_deferred);
    pet_p4_asset_transfer_process(&state,2);assert(!cue_deferred);return 0;
  }
  if(!strcmp(c,"cue-state-and-quarantine")){
    state.asset_transfer_active=true;pet_p4_asset_transfer_process(&state,1);assert(cue_deferred);
    state.asset_transfer_active=false;g_appearance_session.recovery_blocked=true;
    pet_p4_asset_transfer_process(&state,2);assert(cue_deferred);
    g_appearance_session.recovery_blocked=false;g_appearance_session.operation_active=true;
    pet_p4_asset_transfer_process(&state,3);assert(cue_deferred);
    g_appearance_session.operation_active=false;g_raw_asset_chunk.data=malloc(1);
    pet_p4_asset_transfer_process(&state,4);assert(cue_deferred);reset_raw_asset_chunk();
    g_asset_transfer_slot=0;pet_p4_asset_transfer_process(&state,5);assert(cue_deferred);
    g_asset_transfer_slot=-1;pet_p4_asset_transfer_process(&state,6);assert(!cue_deferred);return 0;
  }
  if(!strncmp(c,"header-",7)){
    begin_serial();admission_delays=0;unsigned writes=write_calls,erases=erase_calls;
    pet_resource_realtime_t realtime={0};
    if(!strncmp(c,"header-busy-",12)){
      assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&admission_owner)==PET_RESOURCE_OK);
      assert(pet_resources_device_acquire(admission_owner,PET_RESOURCE_STORAGE_WORK,1,&admission_lease)==PET_RESOURCE_OK);
      admission_release=!strcmp(c,"header-busy-grant");admission_same_id=!strcmp(c,"header-busy-same-id-new-generation");
      admission_owner_change=!strcmp(c,"header-busy-owner-change")||admission_same_id;
      admission_route_change=!strcmp(c,"header-busy-route-change");
    }
    if(!strcmp(c,"header-protected"))assert(pet_resources_realtime_begin(&broker,&realtime)==PET_RESOURCE_OK);
    if(!strcmp(c,"header-foreign"))strcpy(g_appearance_session.id,"foreign");
    fail_raw_buffer=!strcmp(c,"header-no-memory");fail_capture_alloc=!strcmp(c,"header-capture-invalid")||!strcmp(c,"header-preempt-capture-invalid");
    header_preempt=!strcmp(c,"header-lease-preempt")||!strcmp(c,"header-preempt-capture-invalid");
    if(!strcmp(c,"header-body-before-grant"))header_fault=1;
    if(!strcmp(c,"header-revision-invalid"))header_fault=2;
    size_t size=!strcmp(c,"header-invalid")?65537:8192;
    if(!strcmp(c,"header-active-zero-remaining")){
      g_raw_asset_chunk.data=malloc(4);assert(g_raw_asset_chunk.data);g_raw_asset_chunk.active=true;
      g_raw_asset_chunk.expected_size=g_raw_asset_chunk.received_size=4;
    }
    char h[256];snprintf(h,sizeof(h),"{\"transferId\":\"tx\",\"path\":\"p4/a.h264\",\"index\":\"42\",\"size\":%zu,\"checksum\":\"0000000000000000\"}",size);
    if(admission_route_change){
      g_appearance_session.sender=pet_p4_native_usb_send_json_line;g_appearance_session.route=native_route_context();
      g_asset_sender=g_appearance_session.sender;g_asset_route=(void*)g_appearance_session.route;
      cJSON*p=body(h);handle_asset_topic(&state,"asset/raw-chunk",p,g_asset_sender,g_asset_route);cJSON_Delete(p);
      assert(!acks&&!state.asset_transfer_active&&!g_raw_asset_chunk.active);
      assert(write_calls==writes&&erase_calls==erases);
      assert(pet_resources_finish(&broker,admission_lease)==PET_RESOURCE_FINISH_CURRENT);
      assert(pet_resources_owner_revoke(&broker,admission_owner));assert(json_allocations==json_frees);return 0;
    }
    serial("asset/raw-chunk",h);
    if(!strcmp(c,"header-already-granted")){
      assert(positive_acks==1&&g_raw_asset_chunk.active);unsigned char*owned=g_raw_asset_chunk.data;
      serial("asset/raw-chunk",h);assert(positive_acks==1&&negative_acks==1&&g_raw_asset_chunk.data==owned);
      cJSON*ack=body(last_ack_line);const cJSON*p=cJSON_GetObjectItemCaseSensitive(ack,"payload");
      assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"rawIdle")));
      assert(!strcmp(json_string(p,"errorCode"),"raw_already_receiving"));cJSON_Delete(ack);
      reset_raw_asset_chunk();assert(write_calls==writes&&erase_calls==erases);return 0;
    }
    assert(write_calls==writes&&erase_calls==erases); /* Header never writes the body. */
    if(!strcmp(c,"header-busy-grant")){
      assert(admission_delays==1&&positive_acks==1&&g_raw_asset_chunk.active);
    }else{
      assert(negative_acks==1&&!positive_acks);
      cJSON*ack=body(last_ack_line);const cJSON*p=cJSON_GetObjectItemCaseSensitive(ack,"payload");
      assert(!strcmp(json_string(p,"rawReadyProof"),"p4-raw-ready-proof-v1"));
      assert(!strcmp(json_string(p,"boardDeviceId"),"synthetic-board"));
      assert(!strcmp(json_string(p,"bootId"),"0000000000000001"));
      assert(!strcmp(json_string(p,"transferId"),"tx")&&!strcmp(json_string(p,"index"),"42"));
      if(!strcmp(c,"header-body-before-grant")||!strcmp(c,"header-revision-invalid")||!strcmp(c,"header-active-zero-remaining")){
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"rawIdle")));
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"headerAccepted")));
        if(strcmp(c,"header-active-zero-remaining")){
          assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"phaseCurrent")));
          assert(!strcmp(json_string(p,"errorCode"),"phase_outcome_unknown"));
          assert(g_appearance_session.recovery_blocked&&g_raw_asset_chunk.data);
        }
      }else{
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"rawIdle")));
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"headerAccepted")));
        assert(json_u64(p,"rawRemaining",999)==0);
      }
      if(!strcmp(c,"header-busy-owner-change")||!strcmp(c,"header-busy-same-id-new-generation")){
        assert(!state.asset_transfer_active);
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"phaseCurrent")));
        assert(!strcmp(json_string(p,"errorCode"),"phase_outcome_unknown"));
      }
      if(!strcmp(c,"header-busy-owner-change")||!strcmp(c,"header-foreign")){
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"ownerMatched")));
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"phaseCurrent")));
      }
      if(!strcmp(c,"header-lease-preempt")||!strcmp(c,"header-capture-invalid")||!strcmp(c,"header-preempt-capture-invalid")){
        assert(!g_raw_asset_chunk.active&&!g_raw_asset_chunk.data&&!g_appearance_session.recovery_blocked);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"phaseCurrent")));
        assert(!strcmp(json_string(p,"errorCode"),"phase_admission_rejected"));
        unsigned bits=(unsigned)json_u64(p,"phaseReasonBits",0);
        if(strcmp(c,"header-capture-invalid"))assert(bits&(1u<<7));
        if(fail_capture_alloc)assert(bits&(1u<<10));
        assert(!strcmp(json_string(p,"phaseFirstCause"),fail_capture_alloc?"ready_capture_failed":"storage_lease_not_current"));
      }
      if(!strcmp(c,"header-busy-timeout")){assert(admission_delays==100);assert(ack_owner_pins>0);}
      if(!strcmp(c,"header-no-memory")||!strcmp(c,"header-invalid"))assert(ack_owner_pins>0);
      if(!strcmp(c,"header-protected")||!strcmp(c,"header-no-memory")||!strcmp(c,"header-invalid"))assert(!admission_delays);
      cJSON_Delete(ack);
    }
    if(admission_lease.serial){assert(pet_resources_finish(&broker,admission_lease)==PET_RESOURCE_FINISH_CURRENT);assert(pet_resources_owner_revoke(&broker,admission_owner));}
    if(realtime.serial)assert(pet_resources_realtime_finish(&broker,realtime));
    if(header_realtime.serial)assert(pet_resources_realtime_finish(&broker,header_realtime));
    fail_raw_buffer=fail_capture_alloc=false;reset_raw_asset_chunk();
    pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.leases[PET_RESOURCE_STORAGE_WORK]&&!usage.live_owners);
    assert(json_allocations==json_frees);return 0;
  }
  if(!strcmp(c,"query-idle")||!strcmp(c,"query-reborrow-fail")){
    assert(!cue_deferred);
    if(!strcmp(c,"query-reborrow-fail"))fail_borrow=2;
    cJSON*q=body("{\"transferId\":\"q\"}");send_asset_slot_query_ack(&state,q,sender,NULL,"q");cJSON_Delete(q);
    assert(!g_appearance_session.operation_active&&!state.asset_transfer_active);
    assert(acks==(!fail_borrow));
    pet_resource_usage_t u;pet_resources_usage(&broker,&u);assert(!u.leases[PET_RESOURCE_STORAGE_WORK]);assert(json_allocations==json_frees);return 0;
  }
  if(!strcmp(c,"serial-begin-erase"))fail_erase=true;
  if(!strcmp(c,"serial-begin-gc"))fail_gc=true;
  if(!strncmp(c,"serial-begin",12)){
    serial("asset/begin","{\"transferId\":\"tx\",\"totalBytes\":131072,\"rawBytes\":131072}");
    assert(acks==1);if(fail_erase||fail_gc){assert(!positive_acks);fail_erase=fail_gc=false;assert_quarantine();}else assert(positive_acks&&erase_calls&&state.asset_transfer_active);
  }else if(!strncmp(c,"serial-",7)||!strncmp(c,"raw-",4)){
    begin_serial();
    if(!strcmp(c,"serial-admission")){
      unsigned before=io_calls;bool previous_probe=probe;probe=true;
      cJSON*p=body("{\"transferId\":\"tx\"}");
      const char*names[]={"asset/file","asset/raw-chunk","asset/chunk","asset/commit","asset/patch-commit","asset/abort"};
      for(unsigned k=0;k<6;k++)handle_asset_topic(&state,names[k],p,sender,(void*)99);
      assert(before==io_calls&&g_appearance_session.active);
      g_raw_asset_chunk.active=true;handle_asset_topic(&state,"asset/commit",p,sender,NULL);g_raw_asset_chunk.active=false;
      assert(before==io_calls);cJSON_Delete(p);probe=previous_probe;assert(json_allocations==json_frees);return 0;
    }
    if(!strcmp(c,"serial-raw-oom")){
      fail_alloc=true;serial("asset/raw-chunk","{\"transferId\":\"tx\",\"path\":\"p4/a.h264\",\"index\":\"0\",\"size\":4,\"checksum\":\"0000000000000000\"}");
      assert(!g_raw_asset_chunk.active&&!g_appearance_session.recovery_blocked&&!positive_acks);fail_alloc=false;assert(json_allocations==json_frees);return 0;
    }
    if(!strncmp(c,"raw-",4)){
      size_t bytes=!strcmp(c,"raw-65536")?65536:8192;unsigned char*data=malloc(bytes);assert(data);memset(data,0x35,bytes);
      held=false;unsigned long long hash=fnv1a64_bytes(data,bytes);held=true;
      char h[512];snprintf(h,sizeof(h),"{\"transferId\":\"tx\",\"path\":\"p4/a.h264\",\"index\":\"0\",\"size\":%zu,\"checksum\":\"%016llx\"}",bytes,hash);
      /* Match main transport route, not a fixture-only sender. */
      g_appearance_session.sender=transport_send_line;g_asset_sender=transport_send_line;
      cJSON*rp=body(h);handle_asset_topic(&state,"asset/raw-chunk",rp,transport_send_line,NULL);cJSON_Delete(rp);assert(acks==1&&positive_acks);reset_counts();
      task=(void*)3;pet_appearance_commit_io_t io={begin,end,current,(void*)3};
      pet_resource_owner_t foreign={0};pet_resource_lease_t foreign_lease={0};
      if(!strcmp(c,"raw-storage-busy")){
        assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&foreign)==PET_RESOURCE_OK);
        assert(pet_resources_device_acquire(foreign,PET_RESOURCE_STORAGE_WORK,1,&foreign_lease)==PET_RESOURCE_OK);
      }
      if(!strcmp(c,"raw-write-fail"))fail_write=true;
      if(!strcmp(c,"raw-stale-revision"))mode=2;
      char line_buffer[64]={0},other_buffer[64]={0};size_t line_used=0,other_used=0;bool raw_mode=false,other_raw=false;held=false;
      if(!strcmp(c,"raw-route")){
        consume_transport_bytes((const uint8_t*)"{}\n",3,other_buffer,&other_used,sizeof(other_buffer),&other_raw,(void*)77);
        assert(queued==1&&!g_raw_asset_chunk.received_size&&!other_raw);
        consume_transport_bytes(data,512,line_buffer,&line_used,sizeof(line_buffer),&raw_mode,NULL);
        assert(g_raw_asset_chunk.received_size==512&&!acks);
        consume_transport_bytes((const uint8_t*)"{}\n",3,other_buffer,&other_used,sizeof(other_buffer),&other_raw,(void*)77);
        assert(queued==2&&g_raw_asset_chunk.received_size==512&&!other_raw);
      }
      size_t already=!strcmp(c,"raw-route")?512:0;
      consume_transport_bytes(data+already,bytes-already,line_buffer,&line_used,sizeof(line_buffer),&raw_mode,NULL);
      if(!strcmp(c,"raw-route")){consume_transport_bytes((const uint8_t*)"{}\n",3,line_buffer,&line_used,sizeof(line_buffer),&raw_mode,NULL);assert(queued==3&&!raw_mode);}
      assert(!held);held=true;
      assert(acks==1&&frames&&protocol_window&&query_window);
      if(!strcmp(c,"raw-storage-busy")){
        assert(hash_calls==1&&negative_acks&&!g_raw_asset_chunk.active&&!g_appearance_session.recovery_blocked);
        (void)pet_resources_device_finish(foreign_lease);pet_resources_device_revoke(foreign);
        free(data);assert(json_allocations==json_frees);return 0;
      }
      assert(hash_calls>=2);
      if(fail_write||mode){assert(negative_acks);fail_write=false;assert_quarantine();}
      else assert(positive_acks); /* removed below; opaque raw-module state isn't inspected */
      free(data);
    }else if(!strcmp(c,"serial-chunk")){
      serial("asset/chunk","{\"transferId\":\"tx\",\"path\":\"p4/manifest.json\",\"index\":\"0\",\"data\":\"AQIDBA==\",\"size\":4}");assert(acks==1&&positive_acks);
    }else if(!strcmp(c,"serial-file-close")){
      serial("asset/chunk","{\"transferId\":\"tx\",\"path\":\"p4/manifest.json\",\"index\":\"0\",\"data\":\"AQIDBA==\",\"size\":4}");reset_counts();fail_close=true;
      serial("asset/file","{\"transferId\":\"tx\",\"path\":\"p4/manifest.json\",\"size\":4}");assert(acks==1&&negative_acks);fail_close=false;assert_quarantine();
    }else if(!strcmp(c,"serial-file-ok")){
      serial("asset/chunk","{\"transferId\":\"tx\",\"path\":\"p4/manifest.json\",\"index\":\"0\",\"data\":\"AQIDBA==\",\"size\":4}");reset_counts();
      serial("asset/file","{\"transferId\":\"tx\",\"path\":\"p4/manifest.json\",\"size\":4}");assert(acks==1&&positive_acks);
    }else if(!strcmp(c,"serial-abort")){
      serial("asset/abort","{\"transferId\":\"tx\"}");assert(acks==1&&positive_acks&&!state.asset_transfer_active&&!g_appearance_session.active);
    }else if(!strcmp(c,"serial-abort-fail")){
      fail_erase=true;serial("asset/abort","{\"transferId\":\"tx\"}");assert(acks==1&&negative_acks);fail_erase=false;assert_quarantine();
    }else if(!strcmp(c,"serial-idle")){
      unsigned before=io_calls;pet_p4_asset_transfer_process(&state,90000);assert(before==io_calls);clock_us=90000000;
      held=false;asset_idle_worker();assert(!held);held=true;assert(trace_begins==1&&trace_ends==1&&!g_worker_locked_topic_hash);assert(!g_appearance_session.active&&!state.asset_transfer_active&&!acks);
    }else if(!strcmp(c,"serial-stat")){
      serial("asset/stat","{\"transferId\":\"tx\",\"path\":\"p4/absent.json\"}");assert(acks==1&&negative_acks&&!g_appearance_session.recovery_blocked);
    }else assert(!"unknown serial case");
  }else if(!strncmp(c,"native-",7)){
    task=(void*)2;
    const char*h="{\"transferId\":\"tx\",\"path\":\"p4/a.h264\",\"size\":4,\"checksum\":\"be7a5e775165785d\",\"rawBytes\":131072}";
    if(!strncmp(c,"native-fs-",10))h="{\"transferId\":\"tx\",\"path\":\"p4/test.bin\",\"size\":4,\"checksum\":\"be7a5e775165785d\",\"rawBytes\":131072}";
    if(!strcmp(c,"native-stale-frame")){g_native_dispatch_connection=0;native(PET_P4_NATIVE_KIND_FILE_BEGIN,h);assert(!acks&&!io_calls&&!g_appearance_session.active);assert(json_allocations==json_frees);return 0;}
    if(!strcmp(c,"native-begin-erase"))fail_erase=true;
    if(!strcmp(c,"native-fs-begin-close"))fail_close=true;
    native(PET_P4_NATIVE_KIND_FILE_BEGIN,h);
    assert(acks==1);if(fail_erase||fail_close){assert(negative_acks);fail_erase=fail_close=false;assert_quarantine();}
    else {
      assert(positive_acks);reset_counts();
      if(!strcmp(c,"native-data-fail"))fail_write=true;
      if(!strcmp(c,"native-disconnect"))mode=1;
      if(!strcmp(c,"native-fs-data-close")||!strcmp(c,"native-fs-data-write")){
        if(!strcmp(c,"native-fs-data-close"))fail_close=true;else fail_write=true;
        const uint8_t d[4]={1,2,3,4};handle_complete_frame(PET_P4_NATIVE_KIND_FILE_DATA,d,4);
        assert(acks==1&&negative_acks&&g_appearance_session.recovery_blocked);
      }else if(!strncmp(c,"native-fs-",10)){
        const uint8_t d[4]={1,2,3,4};handle_complete_frame(PET_P4_NATIVE_KIND_FILE_DATA,d,4);reset_counts();
        if(!strcmp(c,"native-fs-rename-fail"))fail_rename=true;
        if(!strcmp(c,"native-fs-close-fail"))fail_close=true;
        native(PET_P4_NATIVE_KIND_FILE_END,"{\"transferId\":\"tx\",\"path\":\"p4/test.bin\"}");
        assert(acks==1);if(fail_rename){assert(negative_acks&&g_appearance_session.recovery_blocked);}else if(fail_close){assert(negative_acks);}else assert(positive_acks&&g_native_files==1&&rename_calls==1);
      }else if(!strcmp(c,"native-invalid-existing")){
        uint64_t generation=g_appearance_session.generation;unsigned old=io_calls;
        native(PET_P4_NATIVE_KIND_FILE_BEGIN,"{\"transferId\":\"tx\",\"path\":\"../bad\",\"size\":4,\"checksum\":\"0000000000000000\"}");
        assert(acks==1&&negative_acks&&io_calls==old&&g_appearance_session.generation==generation&&!g_native_failed&&g_current_path[0]);
      }else if(!strncmp(c,"native-data",11)||!strcmp(c,"native-disconnect")){
        const uint8_t d[4]={1,2,3,4};handle_complete_frame(PET_P4_NATIVE_KIND_FILE_DATA,d,4);
        if(fail_write||mode){assert(g_appearance_session.recovery_blocked);fail_write=false;}
        else assert(!acks&&g_received_size==4);
      }else if(!strcmp(c,"native-abort")){
        native(PET_P4_NATIVE_KIND_JSON,"{\"topic\":\"asset/abort\",\"payload\":{\"transferId\":\"tx\"}}");assert(acks==1&&positive_acks&&!g_appearance_session.active);
      }else if(!strcmp(c,"native-idle")){
        unsigned before=io_calls;pet_p4_native_usb_process(90000);assert(before==io_calls);clock_us=90000000;native_idle_cleanup_worker();assert(!g_appearance_session.active&&!acks);
      }else if(strcmp(c,"native-begin"))assert(!"unknown native case");
    }
  }else assert(!"unknown case");
  assert(held&&frames>0);
  pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.leases[PET_RESOURCE_STORAGE_WORK]&&!usage.live_owners);
  /* Evidence output contains case/counters only, never content/device identity. */
  printf("case=%s PASS frames=%u io=%u ack=%u hash=%u\n",c,frames,io_calls,acks,hash_calls);
  assert(json_allocations==json_frees);return 0;
}
