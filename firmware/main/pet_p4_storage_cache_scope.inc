/* One fixed native storage parent, included once after its real lease state.
 * No package/filename policy or grant. Calls are serialized by the existing
 * command lane; the observer retains physical in-flight pins independently. */
#if PET_P4_STORAGE_CACHE_SCOPE_ENABLED
static struct {
  const pet_storage_scope_t *scope;
  uint64_t job_serial;
  uint32_t generation;
} g_storage_cache_scope;
static void storage_cache_scope_begin(pet_storage_scope_t *scope){
  if(g_storage_cache_scope.scope||!scope||!scope->active||scope->lane!=&g_storage_lane
      ||g_storage_lane.scope_owner!=scope||!g_storage_lane.occupied
      ||scope->job_serial!=g_storage_lane.job_serial||scope->command_serial!=g_storage_lane.command_serial
      ||(g_storage_lane.owner&&(!scope->owner||g_storage_lane.owner(g_storage_lane.context)!=scope->owner)))return;
  uint32_t generation=pet_storage_io_begin(PET_STORAGE_IO_MAX_WINDOW_US);
  /* Busy/not-ready, including an explicit diagnostic window, only bypasses
   * caching. Never adopt, reset or stop that other observer's generation. */
  if(generation){g_storage_cache_scope.scope=scope;g_storage_cache_scope.job_serial=scope->job_serial;g_storage_cache_scope.generation=generation;}
}
static void storage_cache_scope_finish(const pet_storage_scope_t *scope){
  /* Call only AFTER the actual scope finish proved physical completion and
   * reacquired the same command lock. A copied/stale/failed finish owns nothing. */
  if(!scope||scope->active||scope->lane!=&g_storage_lane||g_storage_lane.occupied||g_storage_lane.scope_owner
      ||g_storage_cache_scope.scope!=scope||g_storage_cache_scope.job_serial!=scope->job_serial)return;
  uint32_t generation=g_storage_cache_scope.generation;
  memset(&g_storage_cache_scope,0,sizeof(g_storage_cache_scope));
  (void)pet_storage_io_stop(generation); /* Exact generation, no wait/retry. */
}
#endif
