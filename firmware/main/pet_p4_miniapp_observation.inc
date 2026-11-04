/* Query-only canonical FNV-1a32 evidence, not an integrity/security primitive.
 * u32 fields are LE; strings include NUL. No IO, save/flush or VM pump. */
static uint32_t observation_byte(uint32_t h,uint8_t v){return (h^v)*UINT32_C(16777619);}
static uint32_t observation_u32(uint32_t h,uint32_t v){for(unsigned i=0;i<4;i++)h=observation_byte(h,(uint8_t)(v>>(8*i)));return h;}
static uint32_t observation_text(uint32_t h,const char *s){do{h=observation_byte(h,(uint8_t)*s);}while(*s++);return h;}
bool pet_p4_miniapp_observe(pet_p4_miniapp_observation_t *out,pet_p4_miniapp_view_t *view){
  if(!out||!view)return false;memset(out,0,sizeof(*out));
#if PET_COMPONENT_PROGRAM_ENABLED
  /* Wide snapshot is private caller scratch, never a 1200-byte stack frame. */
  pet_program_snapshot_t *vm=heap_caps_calloc(1,sizeof(*vm),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!vm)return false;
#endif
  miniapp_runtime_lock();
  *view=g_runtime.view;
  out->active=g_runtime.active;
  copy_utf8(out->widget_id,sizeof(out->widget_id),g_runtime.widget_id);
  out->instance_generation=g_image_revision;out->view_revision=g_runtime.view.revision;
  uint32_t h=observation_u32(UINT32_C(2166136261),(uint32_t)g_runtime.save_version);
  h=observation_u32(h,g_runtime.save_vars);
  h=observation_text(h,g_runtime.save_version?g_runtime.view.state:"");
  h=observation_text(h,g_runtime.save_page?g_runtime.view.page:"");
  for(unsigned i=0;i<(unsigned)g_runtime.var_count&&i<32;i++)if(g_runtime.save_vars&(UINT32_C(1)<<i)){
    h=observation_text(h,g_runtime.vars[i].name);
    bool integer=g_runtime.vars[i].type==MINIAPP_VAR_INT;h=observation_byte(h,integer?1:2);
    h=integer?observation_u32(h,(uint32_t)g_runtime.vars[i].int_value):observation_text(h,g_runtime.vars[i].string_value);
  }
  out->persist_digest=h;
#if PET_P4_CELLS_V1
  if(g_runtime.cells.enabled){
    const pet_cells_t *c=&g_runtime.cells;out->has_cells=true;out->cols=c->cols;out->rows=c->rows;out->bits=c->bits;
    out->cells_count=(uint16_t)((unsigned)c->cols*c->rows);
    h=observation_u32(UINT32_C(2166136261),out->cols);h=observation_u32(h,out->rows);h=observation_u32(h,out->bits);
    for(unsigned i=0;i<out->cells_count&&i<PET_CELLS_MAX;i++)h=observation_byte(h,c->values[i]);
    out->cells_digest=h;
  }
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
  if(g_runtime.active&&g_runtime.program.enabled){
#if PET_COMPONENT_BACKGROUND_ENABLED
    pet_program_registration_t registration={0};
    if(g_background_mutex){
      xSemaphoreTake(g_background_mutex,portMAX_DELAY);
      if(background_matches_under_registry(&g_runtime,&registration))
        out->has_vm=pet_program_registry_snapshot(&g_background_programs,registration,vm);
      xSemaphoreGive(g_background_mutex);
    }
#endif
    if(!out->has_vm&&g_component_program_initialized&&g_component_program.revision==g_image_revision)
      out->has_vm=component_program_snapshot_locked(vm);
    if(out->has_vm&&vm->cells<=PET_PROGRAM_CELLS){
      out->vm_cells=vm->cells;out->vm_schema=vm->schema;out->vm_revision=vm->revision;
      h=observation_u32(UINT32_C(2166136261),vm->schema);h=observation_u32(h,vm->cells);
      for(unsigned i=0;i<vm->cells;i++)h=observation_u32(h,(uint32_t)vm->values[i]);
      for(unsigned i=0;i<8;i++)h=observation_u32(h,(uint32_t)vm->outputs[i]);
      out->vm_digest=h;
    }else out->has_vm=false;
  }
#endif
  miniapp_runtime_unlock();
#if PET_COMPONENT_PROGRAM_ENABLED
  heap_caps_free(vm);
#endif
  if(view->app_host)pet_app_host_view(view,esp_timer_get_time()/1000ULL);
  pet_p4_widget_data_view(view->data_source,view->data_page,esp_timer_get_time()/1000ULL,&view->data);
  return true;
}
