/* Included ONCE by pet_p4_miniapp.c. Native, explicit installed-program parent.
 * Lock order: model -> registry -> bounded save/broker metadata. Checkpoint
 * serialization holds registry only, never model during encode/cache update.
 * Disk preparation belongs to the existing unlocked protocol storage lane.
 * Ordinary private background profile retains ONE global PROGRAM lease. The
 * separately gated unified parent also dispatches the fixed foreground child
 * from this exact scheduler, under the same mutex and aggregate turn budget.
 */
#if PET_COMPONENT_BACKGROUND_ENABLED
#if PET_PROGRAM_PHASE_TRACE
static pet_program_phase_t g_background_peak_phase;
#if PET_P4_QUERY_OBSERVATION
static pet_p4_query_observation_t g_background_peak_query;
static pet_widget_trace_lane_snapshot_t g_background_peak_save;
bool pet_p4_miniapp_program_peak_context(pet_p4_query_observation_t *query,pet_widget_trace_lane_snapshot_t *save){
  if(!query||!save||!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);*query=g_background_peak_query;*save=g_background_peak_save;
  xSemaphoreGive(g_background_mutex);return true;
}
#endif
bool pet_p4_miniapp_program_peak(pet_program_phase_t *out){
  if(!out||!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);*out=g_background_peak_phase;
  xSemaphoreGive(g_background_mutex);return true;
}
#endif
_Static_assert(PET_P4_MINIAPP_BACKGROUND_MAX==PET_PROGRAM_REGISTRY_SLOTS,"background status capacity must match native table");
static bool background_init(void){
  if(g_background_mutex){
    xSemaphoreTake(g_background_mutex,portMAX_DELAY);bool ok=g_background_ready;
    for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++)if(g_background_programs.entries[i].occupied)ok=false;
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
    if(g_component_program_initialized&&(g_component_program.live||g_component_program.retiring||g_component_program.scheduled))ok=false;
#endif
    xSemaphoreGive(g_background_mutex);return ok; /* Never reset live storage. */
  }
  g_background_mutex=xSemaphoreCreateMutex();if(!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  g_background_ready=pet_program_registry_init(&g_background_programs,pet_program_host_device_init);
  xSemaphoreGive(g_background_mutex);return g_background_ready;
}
static bool background_matches_under_registry(const miniapp_runtime_t *r,pet_program_registration_t *h){
  if(!g_background_ready||!r||!r->active||!r->program.enabled
      ||!pet_program_registry_find(&g_background_programs,r->widget_id,h))return false;
  const pet_program_registry_entry_t *e=&g_background_programs.entries[h->slot];
  if(memcmp(e->content,r->program_content,sizeof(e->content))||memcmp(&e->config,&r->program,sizeof(e->config))){*h=(pet_program_registration_t){0};return false;}
  return true;
}
static bool background_current_locked(const miniapp_runtime_t *r,pet_program_registration_t *h){
  if(!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);bool ok=background_matches_under_registry(r,h);
  xSemaphoreGive(g_background_mutex);return ok;
}
static bool background_seed_locked(miniapp_runtime_t *r,bool *found){
  *found=false;if(!g_background_mutex||!r||!r->program.enabled)return true;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t h={0};bool ok=true;
  if(g_background_ready&&pet_program_registry_find(&g_background_programs,r->widget_id,&h)){
    *found=true;
    const pet_program_registry_entry_t *e=&g_background_programs.entries[h.slot];
    pet_program_snapshot_t s;
    ok=!memcmp(e->content,r->program_content,sizeof(e->content))&&!memcmp(&e->config,&r->program,sizeof(e->config))
      &&pet_program_registry_snapshot(&g_background_programs,h,&s)&&pet_component_program_seed_valid(&r->program,&s);
    if(ok){
      r->program_seed=s;r->program_seed_ready=true;r->program_background_owned=true;
      ok=component_program_apply_seed(r);
      /* Publication must include the current UI projection, not the parse-time
       * dashboard. No pump/timer runs here. Returning from activation before
       * the next main turn must not expose revision N with the old value. */
      if(ok)refresh_view(r);
    }
  }
  xSemaphoreGive(g_background_mutex);return ok;
}
static bool background_project_locked(miniapp_runtime_t *r){
  if(!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t h={0};
  bool found=background_matches_under_registry(r,&h);
  if(found){
    int32_t base[8],values[8];pet_program_snapshot_t snapshot;component_program_values(r,base);
    if(pet_program_registry_view(&g_background_programs,h,base,values,&snapshot)){
      bool changed=false;
      for(unsigned i=0;i<r->var_count;i++)if(r->vars[i].type==MINIAPP_VAR_INT&&r->vars[i].int_value!=values[i]){r->vars[i].int_value=values[i];changed=true;}
      r->program_seed=snapshot;r->program_seed_ready=true;r->program_needs_start=false;
      r->program_background_owned=true;
      r->program_suspended=g_background_programs.entries[h.slot].stopped;
#if PET_P4_CELLS_V1
      bool cells_changed=false;
      if(r->cells.enabled)(void)pet_cells_import(&r->cells,snapshot.values,(unsigned)r->cells.cols*r->cells.rows,&cells_changed);
      changed|=cells_changed;
#endif
      if(changed){g_viewport_epoch++;refresh_view(r);}
    }
  }
  xSemaphoreGive(g_background_mutex);return found;
}
static bool background_action_enabled_locked(const miniapp_runtime_t *r){
  if(!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t h={0};
  bool ok=background_matches_under_registry(r,&h);
  if(ok){const pet_program_registry_entry_t *e=&g_background_programs.entries[h.slot];ok=!e->stopped&&pet_component_program_session_can_enqueue(&e->session,e->serial);}
  xSemaphoreGive(g_background_mutex);return ok;
}
static bool background_status_current_locked(pet_p4_miniapp_program_status_t *out){
  if(!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t h={0};
  bool found=background_matches_under_registry(&g_runtime,&h);
  if(found){const pet_program_registry_entry_t *e=&g_background_programs.entries[h.slot];const pet_component_program_session_t *s=&e->session;
    out->phase=(uint8_t)s->phase;out->live=s->live;out->retiring=s->retiring;
    out->slice_budget_us=(uint16_t)s->host.slice_budget_us;out->slice_stalled=s->host.slice_stalled;
    out->last_step_us=g_background_last_us;out->max_step_us=g_background_max_us;
    out->report_valid=s->report_valid&&s->last_report.generation==s->generation;
    if(out->report_valid){out->revision=s->last_report.revision;out->used_fuel=s->last_report.used_fuel;out->result=(uint8_t)s->last_report.result;}
  }
  xSemaphoreGive(g_background_mutex);return found;
}
static bool background_enqueue_locked(const char *action,const int32_t base[8],bool *accepted){
  *accepted=false;
  if(!g_background_mutex)return false;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t h={0};bool found=background_matches_under_registry(&g_runtime,&h);
  if(found)*accepted=pet_program_registry_enqueue(&g_background_programs,h,action,base)==PET_RUNTIME_OK;
  xSemaphoreGive(g_background_mutex);return found; /* Never fall back/replay into foreground. */
}
static void background_process_locked(uint64_t now){
  if(!g_background_mutex)return;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  bool work=false;for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++)work|=g_background_programs.entries[i].occupied;
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
  if(g_component_program_initialized&&g_component_program.retiring)(void)pet_component_program_session_stop(&g_component_program);
  work|=g_component_program_initialized&&g_component_program.scheduled;
  g_background_last_us=0;
#endif
  if(g_background_ready&&work
#if PET_P4_H264_PREFETCH || PET_P4_NATIVE_WORK_WINDOW
      &&g_program_slice_admitted
#endif
      ){
    int64_t begin=esp_timer_get_time();(void)pet_program_registry_step(&g_background_programs,now);
    int64_t elapsed=esp_timer_get_time()-begin;
    g_background_last_us=elapsed<0?0:elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;
#if PET_PROGRAM_PHASE_TRACE
    if(g_background_last_us>g_background_max_us){
      g_background_peak_phase=g_background_programs.last_phase;
      g_background_peak_phase.registry_us=g_background_programs.last_phase.parent_us;
      g_background_peak_phase.parent_us=g_background_last_us;
      g_background_peak_phase.start_us=begin<0?0:(uint64_t)begin;
      g_background_peak_phase.end_us=begin+elapsed<0?0:(uint64_t)(begin+elapsed);
#if PET_P4_QUERY_OBSERVATION
      (void)pet_p4_protocol_query_observation(&g_background_peak_query);
      pet_widget_trace_snapshot_t storage_observed;pet_widget_trace_snapshot(&storage_observed);
      g_background_peak_save=storage_observed.lanes[1];
#endif
    }
#endif
    if(g_background_last_us>g_background_max_us)g_background_max_us=g_background_last_us;
  }
  xSemaphoreGive(g_background_mutex);
}
static bool background_checkpoint_one(void){
  if(!g_background_mutex)return true;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  bool ok=!g_background_ready||pet_program_registry_checkpoint_one(&g_background_programs);
  xSemaphoreGive(g_background_mutex);return ok;
}
static bool background_prepare_remove(const char *id){
  if(!g_background_mutex)return true;
  miniapp_runtime_lock();xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  pet_program_registration_t h={0};bool found=g_background_ready&&pet_program_registry_find(&g_background_programs,id,&h),ok=true;
  if(found){
    pet_program_runtime_result_t stopped=pet_program_registry_stop(&g_background_programs,h);
    ok=stopped==PET_RUNTIME_OK||stopped==PET_RUNTIME_IDLE;
    if(g_runtime.active&&g_runtime.program.enabled&&!strcmp(g_runtime.widget_id,id)){
      pet_program_snapshot_t snapshot;
      if(pet_program_registry_snapshot(&g_background_programs,h,&snapshot)){
        g_runtime.program_seed=snapshot;g_runtime.program_seed_ready=true;g_runtime.program_background_owned=true;
        (void)component_program_apply_seed(&g_runtime);refresh_view(&g_runtime);
      }
      g_runtime.program_suspended=true;g_runtime.program_needs_start=false;
    }
  }
  xSemaphoreGive(g_background_mutex);miniapp_runtime_unlock();
  if(!found)return true;
  for(unsigned i=0;ok&&i<PET_PROGRAM_REGISTRY_SLOTS;i++)ok=background_checkpoint_one();
  if(ok){xSemaphoreTake(g_background_mutex,portMAX_DELAY);ok=pet_program_registry_release(&g_background_programs,h);xSemaphoreGive(g_background_mutex);}
  return ok;
}
static bool background_prepare_replace(miniapp_runtime_t *next){
  if(!g_background_mutex)return true;
  miniapp_runtime_lock();xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  pet_program_registration_t h={0};bool found=g_background_ready&&pet_program_registry_find(&g_background_programs,next->widget_id,&h),same=false,compatible=true;
  if(found){const pet_program_registry_entry_t *e=&g_background_programs.entries[h.slot];
    same=next->program.enabled&&!memcmp(e->content,next->program_content,sizeof(e->content))&&!memcmp(&e->config,&next->program,sizeof(e->config));
    pet_program_snapshot_t seed;
    if(!same&&next->program.enabled)compatible=pet_program_registry_snapshot(&g_background_programs,h,&seed)&&pet_component_program_seed_valid(&next->program,&seed);
  }
  xSemaphoreGive(g_background_mutex);miniapp_runtime_unlock();
  return compatible&&(!found||same||background_prepare_remove(next->widget_id));
}
bool pet_p4_miniapp_background_available(void){
  if(!g_background_mutex)return false;xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  bool ok=g_background_ready;xSemaphoreGive(g_background_mutex);return ok;
}
bool pet_p4_miniapp_background_content(const char *id,uint32_t content[3]){
  if(!content||!safe_widget_id(id)||g_catalog_recovery_blocked||!pet_p4_miniapp_background_available())return false;
  int index=catalog_find(g_catalog,g_catalog_count,id);
  if(index<0||g_catalog[index].resource_state!=1)return false;
  uint32_t copy[3]={g_catalog[index].widget_checksum,g_catalog[index].buttons_checksum,g_catalog[index].sprites_checksum};
  memcpy(content,copy,sizeof(copy));return true;
}
size_t pet_p4_miniapp_background_list(pet_p4_miniapp_background_status_t out[PET_P4_MINIAPP_BACKGROUND_MAX]){
  if(!out||!g_background_mutex)return 0;size_t count=0;
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);
  if(g_background_ready)for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++){
    const pet_program_registry_entry_t *e=&g_background_programs.entries[i];if(!e->occupied)continue;
    pet_p4_miniapp_background_status_t *s=&out[count++];memset(s,0,sizeof(*s));
    copy_utf8(s->widget_id,sizeof(s->widget_id),e->id);s->registration=e->serial;
    s->revision=e->committed.revision;s->phase=(uint8_t)e->session.phase;
    s->live=e->session.live;s->stopped=e->stopped;s->checkpoint_pending=e->checkpoint_pending;
    s->last_step_us=g_background_last_us;s->max_step_us=g_background_max_us;
  }
  xSemaphoreGive(g_background_mutex);return count;
}
bool pet_p4_miniapp_background_stop(const char *id,uint64_t serial,char *error,size_t size){
  if(!id||!serial||!g_background_mutex){set_error(error,size,"background owner unavailable");return false;}
  miniapp_runtime_lock();xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t h={0};
  bool ok=g_background_ready&&pet_program_registry_find(&g_background_programs,id,&h)&&h.serial==serial;
  if(ok){pet_program_runtime_result_t result=pet_program_registry_stop(&g_background_programs,h);ok=result==PET_RUNTIME_OK||result==PET_RUNTIME_IDLE;
    if(g_runtime.program.enabled&&!strcmp(g_runtime.widget_id,id)){g_runtime.program_suspended=true;g_runtime.program_needs_start=false;}}
  xSemaphoreGive(g_background_mutex);miniapp_runtime_unlock();
  if(ok)(void)background_checkpoint_one();else set_error(error,size,"stale or retiring background owner; no later owner was stopped");return ok;
}
/* Caller holds registry mutex. A real physical PROGRAM lease held by another
 * live registration is capacity BUSY, not a guessed classification of any
 * loader/checkpoint/admission failure. Revoked/stale registrations do not count. */
static bool background_other_program_busy(const char *id){
  pet_resource_limit_t limit=pet_resources_limit(PET_RESOURCE_PROGRAM);
  pet_resource_usage_t usage={0};pet_resources_device_usage(&usage);
  if(!limit.leases||usage.leases[PET_RESOURCE_PROGRAM]<limit.leases)return false;
  for(unsigned i=0;i<PET_PROGRAM_REGISTRY_SLOTS;i++){
    const pet_program_registry_entry_t *e=&g_background_programs.entries[i];
    if(e->occupied&&!e->stopped&&e->session.live&&strcmp(e->id,id)
        &&pet_resources_device_current(e->session.host.lease))return true;
  }
  return false;
}
bool pet_p4_miniapp_background_start(const char *id,const uint32_t content[3],uint64_t *registration,char *error,size_t size){
  if(!pet_p4_miniapp_background_available()||!safe_widget_id(id)||!content||!registration||*registration){set_error(error,size,"background program unavailable or invalid request");return false;}
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish)))={0};
  if(storage_audio_busy()||!storage_scope_begin(&storage,false)){set_error(error,size,"audio or component storage is busy");return false;}
  int index=catalog_find(g_catalog,g_catalog_count,id);if(index<0||g_catalog[index].resource_state!=1){set_error(error,size,"verified installed component required");return false;}
  miniapp_catalog_item_t item=g_catalog[index];uint32_t sequence=g_catalog_sequence;
  if(item.widget_checksum!=content[0]||item.buttons_checksum!=content[1]||item.sprites_checksum!=content[2]){set_error(error,size,"installed content changed; request not applied");return false;}
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t existing={0};
  bool found=pet_program_registry_find(&g_background_programs,id,&existing),duplicate=false;
  if(found){const pet_program_registry_entry_t *e=&g_background_programs.entries[existing.slot];
    duplicate=e->content[0]==content[0]&&e->content[1]==content[1]&&e->content[2]==content[2]
      &&!e->stopped&&e->session.live&&pet_resources_device_current(e->session.host.lease);}
  bool program_busy=!duplicate&&background_other_program_busy(id);
  xSemaphoreGive(g_background_mutex);
  if(duplicate){*registration=existing.serial;return true;} /* Same owner/timers, no restart. */
  if(program_busy){set_error(error,size,"PROGRAM_BUSY");return false;}
  if(found&&!background_prepare_remove(id)){set_error(error,size,"prior owner could not retire/checkpoint");return false;}
  if(!save_app_runtime()){set_error(error,size,"current foreground checkpoint unavailable");return false;}
  if(!pet_storage_scope_advance(&storage,PET_STORAGE_PREPARE)||!pet_storage_scope_unlock(&storage))return false;
  miniapp_runtime_t *next=calloc(1,sizeof(*next));if(!next){set_error(error,size,"component staging memory unavailable");return false;}
  bool ok=false;miniapp_catalog_item_t staged=item;
  if(!storage_work_current()||!load_catalog_runtime_item(&staged,next,false,error,size)||!next->program.enabled)goto done;
  pet_program_save_result_t loaded=pet_program_persistence_load_with_lease(id,&next->program.image,&next->program_seed,g_storage_resource_lease);
  if(loaded!=PET_PROGRAM_SAVE_EMPTY&&loaded!=PET_PROGRAM_SAVE_VALUE){set_error(error,size,"program seed unavailable or incompatible; data retained");goto done;}
  if(!storage_work_current()||!pet_storage_scope_lock(&storage)||storage_audio_busy())goto done;
  index=catalog_find(g_catalog,g_catalog_count,id);
  if(index<0||g_catalog_sequence!=sequence||memcmp(&g_catalog[index],&item,sizeof(item)))goto done;
  /* RAM-only reread at publication, never stale preparation data or Flash IO. */
  loaded=pet_program_persistence_load_cached(id,&next->program.image,&next->program_seed);
  if(loaded!=PET_PROGRAM_SAVE_EMPTY&&loaded!=PET_PROGRAM_SAVE_VALUE)goto done;
  miniapp_runtime_lock();bool compatible=true;
  if(g_runtime.active&&g_runtime.program.enabled&&!strcmp(g_runtime.widget_id,id)
      &&!g_runtime.program_background_owned&&g_component_program_initialized&&g_component_program.revision==g_image_revision){
    /* Accepted/inflight foreground events are never silently discarded during
     * mode transfer. This explicit refusal may be retried by the user only. */
    {
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
      bool held __attribute__((cleanup(component_parent_leave)))=component_parent_enter();
      compatible=held;
#endif
      compatible=compatible&&!g_component_program.host.runtime.queued&&!g_component_program.host.runtime.inflight
        &&!g_component_program.host.runtime.report_pending
        &&pet_component_program_session_snapshot(&g_component_program,&next->program_seed)
        &&pet_component_program_seed_valid(&next->program,&next->program_seed);
    }
    if(compatible){component_program_retire_locked();g_runtime.program_needs_start=false;g_runtime.program_suspended=true;}
  }
  miniapp_runtime_unlock();
  if(!compatible||!pet_component_program_seed_valid(&next->program,&next->program_seed)){set_error(error,size,"foreground work pending or seed incompatible; no event replay");goto done;}
  if(!pet_program_persistence_enqueue(id,&next->program_seed)){set_error(error,size,"latest program checkpoint unavailable; data retained");goto done;}
  int32_t base[8];component_program_values(next,base);uint64_t identity[3]={content[0],content[1],content[2]};
  xSemaphoreTake(g_background_mutex,portMAX_DELAY);pet_program_registration_t handle={0};
  pet_program_registry_result_t admitted=pet_program_registry_attach(&g_background_programs,id,identity,&next->program,&next->program_seed,base,true,(uint64_t)esp_timer_get_time()/1000,&handle);
  ok=admitted==PET_REGISTRY_OK||admitted==PET_REGISTRY_ATTACHED;
  if(ok)*registration=handle.serial;
  xSemaphoreGive(g_background_mutex);
  if(ok){
    miniapp_runtime_lock();
    if(g_runtime.active&&g_runtime.program.enabled&&!strcmp(g_runtime.widget_id,id)){
      g_runtime.program_background_owned=true;g_runtime.program_seed=next->program_seed;g_runtime.program_seed_ready=true;
      g_runtime.program_needs_start=false;g_runtime.program_suspended=false;
    }
    miniapp_runtime_unlock();
  }
  if(!ok)set_error(error,size,"background admission refused; existing owners/data retained");
done:
  free(next);return ok;
}
#else
bool pet_p4_miniapp_background_available(void){return false;}
bool pet_p4_miniapp_background_content(const char *id,uint32_t content[3]){(void)id;(void)content;return false;}
size_t pet_p4_miniapp_background_list(pet_p4_miniapp_background_status_t out[PET_P4_MINIAPP_BACKGROUND_MAX]){(void)out;return 0;}
bool pet_p4_miniapp_background_start(const char *id,const uint32_t content[3],uint64_t *registration,char *error,size_t size){(void)id;(void)content;(void)registration;set_error(error,size,"background capability unavailable");return false;}
bool pet_p4_miniapp_background_stop(const char *id,uint64_t registration,char *error,size_t size){(void)id;(void)registration;set_error(error,size,"background capability unavailable");return false;}
#endif
