/* Explicit PC grant, on the existing protocol storage worker only. Never run
 * from the frame loop. Stage private saved alarms without publishing a runtime,
 * starting a program, arming declarations or executing an action. */
typedef struct {
  miniapp_catalog_item_t catalog[PET_P4_MINIAPP_CATALOG_MAX];
  ext_alarm_entry_t pending[PET_CEV_COMPONENTS];
  bool skip[PET_P4_MINIAPP_CATALOG_MAX];
  miniapp_runtime_t runtime;
  miniapp_catalog_item_t loaded;
  char save[PET_APP_SAVE_BYTES];
  char error[128];
  size_t count, pending_count;
} ext_alarm_boot_workspace_t;

bool pet_p4_miniapp_alarm_policy_prepare(bool background_notify) {
  if (!background_notify) { pet_p4_miniapp_alarm_policy(false); return true; }
  if (g_catalog_recovery_blocked || storage_audio_busy()) return false;
  pet_storage_scope_t storage __attribute__((cleanup(storage_scope_finish))) = {0};
  if (!storage_scope_begin(&storage, false)) return false;
  /* A configured command lane must really release the shared state lock.
   * This is not a synchronous native/main-task fallback for disk scanning. */
  if (!storage.may_unlock || !pet_storage_scope_advance(&storage, PET_STORAGE_PREPARE)
      || !pet_storage_scope_unlock(&storage)) return false;
  ext_alarm_boot_workspace_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!w) return false;
  bool ok = false;
  uint32_t sequence = 0;
  uint64_t policy_revision = 0;
  if (!pet_storage_scope_lock(&storage) || !storage_work_current()
      || g_catalog_count > PET_P4_MINIAPP_CATALOG_MAX) goto done;
  sequence = g_catalog_sequence; w->count = g_catalog_count;
  memcpy(w->catalog, g_catalog, w->count * sizeof(w->catalog[0]));
  miniapp_runtime_lock(); ext_lock();
  policy_revision = g_ext_alarm_policy_revision;
  for (size_t i = 0; i < w->count; i++) {
    const char *id = w->catalog[i].widget_id;
    w->skip[i] = (g_runtime.active && !strcmp(g_runtime.widget_id, id)) || ext_alarm_entry(id, false) != NULL;
  }
  ext_unlock(); miniapp_runtime_unlock();
  if (!pet_storage_scope_unlock(&storage)) goto done;
  for (size_t i = 0; i < w->count; i++) {
    miniapp_catalog_item_t *item = &w->catalog[i];
    if (w->skip[i] || item->resource_state != 1) continue;
    if (!storage_work_current() || !safe_widget_id(item->widget_id)) goto done;
    pet_app_store_status_t status = pet_app_store_load_with_lease(item->widget_id, w->save, g_storage_resource_lease);
    if (status == PET_APP_STORE_EMPTY) continue; /* No save never creates a timer. */
    if (status != PET_APP_STORE_VALUE || !pet_form_source_valid(w->save)) goto done;
    cJSON *saved = cJSON_ParseWithOpts(w->save, NULL, true);
    if (!cJSON_IsObject(saved)) { cJSON_Delete(saved); goto done; }
    const cJSON *alarms = cJSON_GetObjectItemCaseSensitive(saved, "alarms");
    bool absent = alarms == NULL;
    bool valid = absent || (cJSON_IsString(alarms) && strlen(alarms->valuestring) < PET_CEV_SAVE_TEXT_MAX);
    bool empty = !absent && valid && !alarms->valuestring[0];
    cJSON_Delete(saved);
    if (!valid) goto done;
    if (absent || empty) continue;
    /* Reuse the installed package checksum + full declaration validator, and
     * the ordinary save-version/state/vars/alarms validator. Neither publishes
     * this temporary runtime nor calls the program/action/extension lifecycle. */
    memset(&w->runtime, 0, sizeof(w->runtime));
    w->loaded = *item;
    if (!load_catalog_runtime_item(&w->loaded, &w->runtime, false, w->error, sizeof(w->error))
        || !w->runtime.save_version || !w->runtime.cext.alarms.count
        || !restore_app_runtime_buffer(&w->runtime, true, w->save)
        || w->pending_count >= PET_CEV_COMPONENTS) goto done;
    ext_alarm_entry_t *entry = &w->pending[w->pending_count++];
    copy_utf8(entry->widget_id, sizeof(entry->widget_id), item->widget_id);
    copy_utf8(entry->title, sizeof(entry->title), w->runtime.view.title);
    entry->alarms = w->runtime.cext.alarms;
  }
  if (!storage_work_current() || !pet_storage_scope_lock(&storage)
      || storage_audio_busy() || g_catalog_recovery_blocked
      || sequence != g_catalog_sequence || w->count != g_catalog_count
      || memcmp(w->catalog, g_catalog, w->count * sizeof(w->catalog[0]))) goto done;
  miniapp_runtime_lock(); ext_lock();
  unsigned missing = 0, available = 0;
  for (unsigned i = 0; i < PET_CEV_COMPONENTS; i++) available += !g_ext_alarms[i].used;
  for (size_t i = 0; i < w->pending_count; i++) {
    const char *id = w->pending[i].widget_id;
    if ((!g_runtime.active || strcmp(g_runtime.widget_id, id)) && !ext_alarm_entry(id, false)) missing++;
  }
  /* Validate the whole RAM commit first. Existing active/parked state always
   * wins; a concurrent disable/cancel/publication cannot be overwritten. */
  if (policy_revision == g_ext_alarm_policy_revision && policy_revision != UINT64_MAX
      && missing <= available && missing <= UINT64_MAX - g_ext_alarm_serial) {
    for (size_t i = 0; i < w->pending_count; i++) {
      ext_alarm_entry_t *staged = &w->pending[i];
      if ((g_runtime.active && !strcmp(g_runtime.widget_id, staged->widget_id))
          || ext_alarm_entry(staged->widget_id, false)) continue;
      ext_alarm_entry_t *entry = ext_alarm_entry(staged->widget_id, true);
      entry->alarms = staged->alarms;
      copy_utf8(entry->title, sizeof(entry->title), staged->title);
    }
    g_ext_background_notify = true; g_ext_alarm_policy_revision++; ok = true;
  }
  ext_unlock(); miniapp_runtime_unlock();
done:
  /* All allocator cleanup outside state/model/ext locks, including failures. */
  if (!storage.unlocked && !pet_storage_scope_unlock(&storage)) {
    /* Ownership failure is unrecoverable by this command; retain the grant. */
    ok = false;
  }
  heap_caps_free(w);
  return ok;
}
