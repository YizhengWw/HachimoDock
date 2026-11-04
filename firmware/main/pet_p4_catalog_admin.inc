/* Included only by miniapp: bounded metadata CAS, no resource or save cleanup.
 * The catalog membership itself is the uninstall tombstone, sealed in the same
 * alternating generation as order/active/cursor, never a second sidecar write. */
static void catalog_digest(char out[9]) {
  uint32_t hash=2166136261U;
  char part[192];
  const char *active=g_runtime.active?g_runtime.widget_id:"";
  const char *cursor=g_catalog_selected<g_catalog_count?g_catalog[g_catalog_selected].widget_id:"";
  snprintf(part,sizeof(part),"%u:%u:%s:%s\n",(unsigned)g_catalog_sequence,
    (unsigned)g_catalog_seed_pending,active,cursor);
  for(size_t i=0;;i++){
    for(const unsigned char *p=(const unsigned char *)part;*p;p++){hash^=*p;hash*=16777619U;}
    if(i==g_catalog_count)break;
    const miniapp_catalog_item_t *item=&g_catalog[i];
    snprintf(part,sizeof(part),"%s:%u:%u:%08x:%08x:%08x:%u:%u:%u\n",item->widget_id,
      item->slot,item->package_generation,item->widget_checksum,item->buttons_checksum,
      item->sprites_checksum,item->resource_state,item->image_count,catalog_item_visible(item));
  }
  snprintf(out,9,"%08x",hash);
}
/* Caller holds the serialized model lane. No flash access, activation, resource
 * decoding, or application text is included in this inspection. */
cJSON *pet_p4_miniapp_catalog_inspect(void){
  cJSON *root=cJSON_CreateObject(),*items=cJSON_CreateArray();
  if(!root||!items){cJSON_Delete(root);cJSON_Delete(items);return NULL;}
  if(!cJSON_AddItemToObject(root,"items",items)){cJSON_Delete(root);cJSON_Delete(items);return NULL;}
  char digest[9];catalog_digest(digest);
  if(!cJSON_AddNumberToObject(root,"schemaVersion",1)
      ||!cJSON_AddStringToObject(root,"engine","p4-catalog-metadata-v1")
      ||!cJSON_AddBoolToObject(root,"ok",!g_catalog_recovery_blocked)
      ||!cJSON_AddBoolToObject(root,"complete",!g_catalog_recovery_blocked)
      ||!cJSON_AddBoolToObject(root,"recoveryBlocked",g_catalog_recovery_blocked)
      ||!cJSON_AddNumberToObject(root,"catalogSeq",g_catalog_sequence)
      ||!cJSON_AddStringToObject(root,"catalogDigest",digest)
      ||!cJSON_AddNumberToObject(root,"physicalCount",g_catalog_count))goto fail;
  const char *active=g_runtime.active?g_runtime.widget_id:NULL;
  const char *cursor=g_catalog_selected<g_catalog_count?g_catalog[g_catalog_selected].widget_id:NULL;
  if(!(active?cJSON_AddStringToObject(root,"activeWidgetId",active):cJSON_AddNullToObject(root,"activeWidgetId"))
      ||!(cursor?cJSON_AddStringToObject(root,"selectedWidgetId",cursor):cJSON_AddNullToObject(root,"selectedWidgetId")))goto fail;
  for(size_t i=0;i<g_catalog_count;i++){
    const miniapp_catalog_item_t *e=&g_catalog[i];cJSON *item=cJSON_CreateObject();
    if(!item)goto fail;
    if(!cJSON_AddItemToArray(items,item)){cJSON_Delete(item);goto fail;}
    char widget[9],buttons[9],sprites[9];
    snprintf(widget,sizeof(widget),"%08x",e->widget_checksum);
    snprintf(buttons,sizeof(buttons),"%08x",e->buttons_checksum);
    snprintf(sprites,sizeof(sprites),"%08x",e->sprites_checksum);
    if(!cJSON_AddStringToObject(item,"id",e->widget_id)
        ||!cJSON_AddBoolToObject(item,"visible",catalog_item_visible(e))
        ||!cJSON_AddNumberToObject(item,"slot",e->slot)
        ||!cJSON_AddNumberToObject(item,"packageGeneration",e->package_generation)
        ||!cJSON_AddStringToObject(item,"widgetChecksum",widget)
        ||!cJSON_AddStringToObject(item,"buttonsChecksum",buttons)
        ||!cJSON_AddStringToObject(item,"spritesChecksum",sprites)
        ||!cJSON_AddStringToObject(item,"resourceState",e->resource_state==1?"ready":e->resource_state==2?"bootstrap":e->resource_state==3?"damaged":"unchecked")
        ||!cJSON_AddNumberToObject(item,"imageCount",e->image_count))goto fail;
  }
  return root;
fail:cJSON_Delete(root);return NULL;
}

/* Return a categorical commit state. Even a failed physical write is unknown:
 * never permit retry on a transport timeout or infer rollback from a bad ACK. */
const char *pet_p4_miniapp_catalog_reconcile(const cJSON *request,char *error,size_t error_size){
  const char *result="not_committed";
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish)))={0};
  if(g_catalog_recovery_blocked||g_staging.active||storage_audio_busy()||!storage_scope_begin(&storage,false)){
    set_error(error,error_size,"catalog unavailable or audio busy");return result;
  }
  const cJSON *seq=cJSON_GetObjectItemCaseSensitive(request,"expectedCatalogSeq");
  const cJSON *remove=cJSON_GetObjectItemCaseSensitive(request,"removeVisibleIds");
  const cJSON *survivors=cJSON_GetObjectItemCaseSensitive(request,"survivorVisibleOrder");
  char before[9];catalog_digest(before);
  if(!cJSON_IsNumber(seq)||seq->valuedouble!=(double)g_catalog_sequence
      ||strcmp(json_string(request,"expectedCatalogDigest"),before)
      ||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"preserveSave"))
      ||!cJSON_IsArray(remove)||!cJSON_IsArray(survivors)
      ||cJSON_GetArraySize(remove)>PET_P4_MINIAPP_CATALOG_MAX
      ||cJSON_GetArraySize(survivors)>PET_P4_MINIAPP_CATALOG_MAX){
    set_error(error,error_size,"stale or invalid catalog plan");return result;
  }
  miniapp_catalog_item_t *next __attribute__((cleanup(catalog_scratch_free)))=calloc(PET_P4_MINIAPP_CATALOG_MAX,sizeof(*next));
  if(!next){set_error(error,error_size,"catalog allocation unavailable");return result;}
  bool removed[PET_P4_MINIAPP_CATALOG_MAX]={0},used[PET_P4_MINIAPP_CATALOG_MAX]={0};
  char active[48]={0},cursor[48]={0};
  if(g_runtime.active)copy_utf8(active,sizeof(active),g_runtime.widget_id);
  if(g_catalog_selected<g_catalog_count)copy_utf8(cursor,sizeof(cursor),g_catalog[g_catalog_selected].widget_id);
  const cJSON *id;size_t removed_count=0,visible_survivors=0;
  cJSON_ArrayForEach(id,remove){
    int at=cJSON_IsString(id)&&safe_widget_id(id->valuestring)?catalog_find(g_catalog,g_catalog_count,id->valuestring):-1;
    if(at<0||removed[at]||!catalog_item_visible(&g_catalog[at])
        ||!strcmp(active,id->valuestring)||!strcmp(cursor,id->valuestring))goto invalid;
    removed[at]=true;removed_count++;
  }
  for(size_t i=0;i<g_catalog_count;i++)if(!removed[i]&&catalog_item_visible(&g_catalog[i]))visible_survivors++;
  if((size_t)cJSON_GetArraySize(survivors)!=visible_survivors)goto invalid;
  cJSON_ArrayForEach(id,survivors){
    int at=cJSON_IsString(id)&&safe_widget_id(id->valuestring)?catalog_find(g_catalog,g_catalog_count,id->valuestring):-1;
    if(at<0||removed[at]||used[at]||!catalog_item_visible(&g_catalog[at]))goto invalid;
    used[at]=true;
  }
#if PET_COMPONENT_BACKGROUND_ENABLED
  /* Metadata-only recovery cannot retire/checkpoint a separate program owner.
   * Protect any registered owner instead of silently leaving an orphan lease. */
  if(removed_count){
    pet_p4_miniapp_background_status_t *owners=calloc(PET_P4_MINIAPP_BACKGROUND_MAX,sizeof(*owners));
    if(!owners)goto invalid;
    size_t owner_count=pet_p4_miniapp_background_list(owners);bool protected=false;
    if(owner_count>PET_P4_MINIAPP_BACKGROUND_MAX)protected=true;
    else for(size_t i=0;i<owner_count;i++){
      int at=catalog_find(g_catalog,g_catalog_count,owners[i].widget_id);
      if(at>=0&&removed[at])protected=true;
    }
    free(owners);if(protected)goto invalid;
  }
#endif
  size_t count=0;int visible_index=0;
  for(size_t i=0;i<g_catalog_count;i++){
    if(removed[i])continue;
    int at=(int)i;
    if(catalog_item_visible(&g_catalog[i])){
      id=cJSON_GetArrayItem(survivors,visible_index++);
      at=catalog_find(g_catalog,g_catalog_count,id->valuestring);
    }
    next[count++]=g_catalog[at];
  }
  if(!removed_count&&!memcmp(next,g_catalog,count*sizeof(*next)))return "unchanged";
  const int generation=g_catalog_file_generation;const uint32_t sequence=g_catalog_sequence;
  if(!pet_storage_scope_advance(&storage,PET_STORAGE_PREPARE)||!pet_storage_scope_unlock(&storage)||!storage_work_current())return result;
  miniapp_catalog_commit_t commit;
  if(!write_catalog_snapshot_at(next,count,active,cursor,false,generation,sequence,&commit)){
    if(g_catalog_recovery_blocked){
      set_error(error,error_size,"catalog write outcome unknown; inspect only, do not replay");return "unknown";
    }
    set_error(error,error_size,"catalog not committed; prior membership retained");return "not_committed";
  }
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_DURABLE);
  bool relocked=pet_storage_scope_lock(&storage);
  bool current=relocked&&storage_work_current();
  char now[9];catalog_digest(now);
  if(!current||strcmp(before,now)){
    g_catalog_recovery_blocked=true;
    /* Quarantine the durable unpublished result, then release the physical job.
     * No further mutation may overwrite either generation before recovery. */
    if(relocked){(void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);(void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);}
    set_error(error,error_size,"catalog durable but publication unknown; do not replay");return "unknown";
  }
  miniapp_runtime_lock();memcpy(g_catalog,next,sizeof(g_catalog));g_catalog_count=count;
  int selected=catalog_find(g_catalog,count,cursor);g_catalog_selected=selected>=0?(size_t)selected:0;
  g_catalog_file_generation=commit.generation;g_catalog_sequence=commit.sequence;g_catalog_seed_pending=false;
  miniapp_runtime_unlock();
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_PUBLISHED);
  (void)pet_storage_scope_advance(&storage,PET_STORAGE_CLEANUP);
  return "committed";
invalid:set_error(error,error_size,"plan must name exact visible survivors; hidden, active, selected and registered programs are protected");return result;
}
