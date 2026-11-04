
  /* Audio-only identity proof uses actual committed raw headers, never video IO. */
  header(1,4096,'a');invalidate_cache_for_test(1);
  put_u64(flash_bytes[1]+104,UINT64_C(0x123456789abcdef0));
  char audio_pack[65];memset(audio_pack,'a',64);audio_pack[64]=0;
  uint32_t proof_size=777;unsigned long long proof_checksum=888;
  unsigned proof_reads=reads;
  assert(pet_p4_raw_assets_stat_for_slot(1,audio_pack,"p4/a.h264",64,&proof_size,&proof_checksum));
  assert(reads==proof_reads+1&&proof_size==64&&proof_checksum==UINT64_C(0x123456789abcdef0));
  proof_reads=reads;
  assert(!pet_p4_raw_assets_stat_for_slot(1,new_pack,"p4/a.h264",64,&proof_size,&proof_checksum));
  assert(!pet_p4_raw_assets_stat_for_slot(1,audio_pack,"p4/a.h264",65,&proof_size,&proof_checksum));
  assert(!pet_p4_raw_assets_stat_for_slot(-1,audio_pack,"p4/a.h264",64,&proof_size,&proof_checksum));
  assert(reads==proof_reads);
  assert(begin_cache_mutation(1));
  assert(!pet_p4_raw_assets_stat_for_slot(1,audio_pack,"p4/a.h264",64,&proof_size,&proof_checksum));
  assert(reads==proof_reads);end_cache_mutation(1);
  /* Capture actual RAM catalog once; later caller memory cannot change proof. */
  static pet_p4_asset_catalog_t catalog;
  catalog.count=2;strcpy(catalog.entries[0].path,"p4/a.h264");catalog.entries[0].stream_bytes=64;
  catalog.entries[1]=catalog.entries[0];
  assert(asset_query_snapshot_videos(&catalog)&&g_asset_query_video_count==1);
  strcpy(catalog.entries[0].path,"p4/changed.h264");
  cJSON *videos=asset_query_video_proof(1,audio_pack);assert(videos&&cJSON_GetArraySize(videos)==1);
  cJSON *row=cJSON_GetArrayItem(videos,0);
  assert(!strcmp(cJSON_GetObjectItem(row,"path")->valuestring,"p4/a.h264"));
  assert(cJSON_GetObjectItem(row,"size")->valuedouble==64);
  assert(!strcmp(cJSON_GetObjectItem(row,"checksum")->valuestring,"123456789abcdef0"));cJSON_Delete(videos);
  assert(!asset_query_video_proof(1,new_pack));
  g_asset_query_videos[0].size=63;assert(!asset_query_video_proof(1,audio_pack));
  catalog.entries[0]=catalog.entries[1];catalog.entries[1].stream_bytes=63;
  assert(!asset_query_snapshot_videos(&catalog));
  catalog.count=25;assert(!asset_query_snapshot_videos(&catalog));
  /* Ambiguous duplicate raw path hash never becomes a video identity proof. */
  assert(load_cache(1));g_caches[1].entries[1]=g_caches[1].entries[0];g_caches[1].entry_count=2;
  assert(!pet_p4_raw_assets_stat_for_slot(1,audio_pack,"p4/a.h264",64,&proof_size,&proof_checksum));
  invalidate_cache_for_test(1);
  puts("PASS exact custom pack/path/size/FNV proof; header-only read; wrong/ambiguous/mutating proof rejected");

  /* Restore the original cache harness initial accounting; its assertions stay unchanged. */
  assert(allocations==frees);allocations=0;frees=0;reads=0;
