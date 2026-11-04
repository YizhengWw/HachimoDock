#include "pet_p4_file_cleanup.h"
#include "pet_p4_widget_trace.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

typedef struct {
  pet_file_cleanup_plan_t *plan;
  pet_file_cleanup_diagnostic_t value;
} cleanup_report_t;

/* One bounded numeric line at prepare return, after closedir and trace scope
 * cleanup. No paths, allocation, extra filesystem calls or observer locks.
 * Preserve errno even if the diagnostic output itself fails. */
static void cleanup_report_end(cleanup_report_t *report) {
  int saved_errno=errno;
  if(report->plan)report->plan->diagnostic=report->value;
#if defined(ESP_PLATFORM) || defined(PET_FILE_CLEANUP_DIAGNOSTICS_TEST)
  const pet_file_cleanup_diagnostic_t *d=&report->value;
  ESP_LOGI("pet-cleanup", "P4 cleanup used=%u reason=%u n=%u entries=%u found=%u err=%d,%d,%d epoch=%llu,%llu obs=%u,%u activity=%u flags=%u attempts=%u first=%u,%d,%u,%u",
    (unsigned)(report->plan&&report->plan->snapshot_used),(unsigned)d->reason,
    (unsigned)d->targets,(unsigned)d->entries,(unsigned)d->found,
    (int)d->open_errno,(int)d->read_errno,(int)d->close_errno,
    (unsigned long long)d->epoch_before,(unsigned long long)d->epoch_after,
    (unsigned)d->observer_before,(unsigned)d->observer_after,
    (unsigned)d->activity,(unsigned)d->flags,(unsigned)d->attempts,
    (unsigned)d->first_reason,(int)d->first_read_errno,
    (unsigned)d->first_entries,(unsigned)d->first_flags);
#endif
  errno=saved_errno;
}

bool pet_file_cleanup_prepare(pet_file_cleanup_plan_t *plan, const char *directory,
  const pet_file_cleanup_target_t *targets, size_t count, pet_file_cleanup_guard_t guard) {
  cleanup_report_t report __attribute__((cleanup(cleanup_report_end)))={
    .plan=plan,.value={.reason=PET_CLEANUP_INVALID_ARGUMENT,.first_reason=UINT8_MAX,
      .targets=(uint8_t)(count>PET_FILE_CLEANUP_MAX_TARGETS?PET_FILE_CLEANUP_MAX_TARGETS+1:count)}};
  pet_file_cleanup_diagnostic_t *d=&report.value;
  if (!plan) return false;
  *plan=(pet_file_cleanup_plan_t){0};
  if (!directory || !targets || !guard.owner_current || count>PET_FILE_CLEANUP_MAX_TARGETS) return false;
  size_t prefix=0;
  while (prefix<PET_FILE_CLEANUP_PATH_BYTES && directory[prefix]) prefix++;
  if (!prefix || prefix==PET_FILE_CLEANUP_PATH_BYTES || directory[prefix-1]=='/') return false;
  d->reason=PET_CLEANUP_INVALID_TARGET;
  for (size_t i=0;i<count;i++) {
    const char *end=memchr(targets[i].path,0,sizeof(targets[i].path));
    if (!end || (size_t)(end-targets[i].path)<=prefix+1
        || memcmp(targets[i].path,directory,prefix) || targets[i].path[prefix]!='/') return false;
    const char *name=targets[i].path+prefix+1;
    if (strchr(name,'/') || !strcmp(name,".") || !strcmp(name,"..")) return false;
    for (size_t j=0;j<i;j++) if (!strcmp(targets[i].path,targets[j].path)) return false;
  }
  if (!guard.owner_current(guard.context)) {
    d->reason=PET_CLEANUP_OWNER_BEFORE;d->flags|=PET_CLEANUP_DIAG_OWNER_LOST;return false;
  }
  *plan=(pet_file_cleanup_plan_t){.targets=targets,.guard=guard,.count=(uint8_t)count,
    .present=count?((UINT32_C(1)<<count)-1):0,.valid=true};
  if (!count) { d->reason=PET_CLEANUP_EMPTY;return true; }
  uint64_t before=0,after=0;
  if (!guard.idle_epoch) { d->reason=PET_CLEANUP_OBSERVER_MISSING;return true; }
  bool initial_idle=guard.idle_epoch(guard.context,&before);
  d->epoch_before=before;d->observer_before=initial_idle?1:2;
  if (!initial_idle) {
    d->reason=PET_CLEANUP_OBSERVER_BEFORE;d->activity|=PET_CLEANUP_ACTIVITY_UNAVAILABLE;return true;
  }
  for(unsigned attempt=0;attempt<2;attempt++) {
    /* Never reset the first epoch. An independent reader/writer between the
     * two scans invalidates the optimization just like one during a scan. */
    if(attempt) {
      if(!guard.owner_current(guard.context)) {
        d->reason=PET_CLEANUP_OWNER_DURING;d->flags|=PET_CLEANUP_DIAG_OWNER_LOST;
        plan->valid=false;return false;
      }
      bool idle=guard.idle_epoch(guard.context,&after);
      d->epoch_after=after;d->observer_after=idle?1:2;
      if(before!=after)d->activity|=PET_CLEANUP_ACTIVITY_EPOCH_CHANGED;
      if(!idle)d->activity|=PET_CLEANUP_ACTIVITY_UNAVAILABLE;
      if(!idle || before!=after) {
        d->reason=idle?PET_CLEANUP_EPOCH_DURING:PET_CLEANUP_OBSERVER_DURING;return true;
      }
    }
    bool complete=false,reliable=true,close_ok=false;
    uint32_t present=0; /* Not OR-ed with any previous scan's partial results. */
    d->reason=PET_CLEANUP_SNAPSHOT;d->flags=0;d->entries=0;d->found=0;
    d->open_errno=0;d->read_errno=0;d->close_errno=0;
    d->attempts++;
  {
    PET_WIDGET_TRACE_SCOPE(scan,PET_WTRACE_FS_SCAN);
    DIR *dir=opendir(directory);
    if (!dir) {
      d->reason=PET_CLEANUP_OPEN_FAILED;d->open_errno=errno;
      if(!guard.owner_current(guard.context)) {
        d->reason=PET_CLEANUP_OWNER_AFTER;d->flags|=PET_CLEANUP_DIAG_OWNER_LOST;plan->valid=false;
      }
      if(!attempt)d->first_reason=d->reason;
      return plan->valid;
    }
    d->flags|=PET_CLEANUP_DIAG_OPENED;
    for (size_t entries=0;entries<=PET_FILE_CLEANUP_MAX_ENTRIES;entries++) {
      /* This is observation, not reservation. Stop scanning at the next
       * bounded directory call if another file actor starts, rather than
       * finishing a now-useless full pass before taking the old path. */
      if (!guard.owner_current(guard.context)) {
        d->reason=PET_CLEANUP_OWNER_DURING;d->flags|=PET_CLEANUP_DIAG_OWNER_LOST;
        reliable=false;break;
      }
      bool idle=guard.idle_epoch(guard.context,&after);
      if (before!=after)d->activity|=PET_CLEANUP_ACTIVITY_EPOCH_CHANGED;
      if (!idle) {
        d->reason=PET_CLEANUP_OBSERVER_DURING;d->activity|=PET_CLEANUP_ACTIVITY_UNAVAILABLE;
        reliable=false;break;
      }
      if (before!=after) { d->reason=PET_CLEANUP_EPOCH_DURING;reliable=false;break; }
      errno=0;
      struct dirent *entry=readdir(dir);
      if (!entry) {
        d->read_errno=errno;complete=errno==0;d->flags|=PET_CLEANUP_DIAG_READ_NULL;
        if(complete)d->flags|=PET_CLEANUP_DIAG_COMPLETE;
        else d->reason=PET_CLEANUP_READ_FAILED;
        break;
      }
      d->entries++;
      if (entries==PET_FILE_CLEANUP_MAX_ENTRIES) {
        d->reason=PET_CLEANUP_ENTRY_LIMIT;reliable=false;break;
      }
      if (!memchr(entry->d_name,0,sizeof(entry->d_name))) {
        d->reason=PET_CLEANUP_NAME_UNTERMINATED;reliable=false;break;
      }
      for (size_t i=0;i<count;i++) if (!strcmp(entry->d_name,targets[i].path+prefix+1)) {
        uint32_t bit=UINT32_C(1)<<i;
        if (present&bit) { d->reason=PET_CLEANUP_DUPLICATE_TARGET;reliable=false; }
        present|=bit;
      }
      if (!reliable) break;
    }
    close_ok=closedir(dir)==0;
    if (!close_ok) {
      d->close_errno=errno;
      if(d->reason==PET_CLEANUP_SNAPSHOT)d->reason=PET_CLEANUP_CLOSE_FAILED;
      reliable=false;
    } else d->flags|=PET_CLEANUP_DIAG_CLOSED_OK;
  }
  d->found=present;
  bool final_owner=guard.owner_current(guard.context);
  /* The extra final observation when already falling back is diagnostic only;
   * it never makes a failed scan eligible or changes per-target remove policy. */
  bool final_idle=guard.idle_epoch(guard.context,&after);
  d->epoch_after=after;d->observer_after=final_idle?1:2;
  if(!final_idle)d->activity|=PET_CLEANUP_ACTIVITY_UNAVAILABLE;
  if(before!=after)d->activity|=PET_CLEANUP_ACTIVITY_EPOCH_CHANGED;
  if (!final_owner || (d->flags&PET_CLEANUP_DIAG_OWNER_LOST)) {
    if(!(d->flags&PET_CLEANUP_DIAG_OWNER_LOST))d->reason=PET_CLEANUP_OWNER_AFTER;
    d->flags|=PET_CLEANUP_DIAG_OWNER_LOST;plan->valid=false;
  }
  if(plan->valid&&complete&&reliable&&!final_idle)d->reason=PET_CLEANUP_OBSERVER_AFTER;
  else if(plan->valid&&complete&&reliable&&before!=after)d->reason=PET_CLEANUP_EPOCH_AFTER;
  if(!attempt) {
    d->first_reason=d->reason;d->first_read_errno=d->read_errno;
    d->first_entries=d->entries;d->first_flags=d->flags;
  }
  if(!plan->valid)return false;
  if (complete && reliable && final_idle && before==after) {
    plan->present=present;plan->snapshot_used=true;
    return true;
  }
  /* IDF 5.5.1 can report EIO at directory EOF after an earlier successful file
   * EOF leaves SPIFFS_ERR_END_OF_OBJECT. Never reinterpret an error as EOF: a new
   * independently complete read is required, under the original ownership and
   * observation baseline. Real recurring read faults still take the old path.
   * close_ok is explicit: closedir failure need not set a nonzero errno. */
  if(!attempt && d->reason==PET_CLEANUP_READ_FAILED && close_ok
      && final_idle && before==after)continue;
  return true;
  }
  return true;
}

bool pet_file_cleanup_remove_at(const pet_file_cleanup_plan_t *plan,size_t index) {
  if (!plan || !plan->valid || index>=plan->count
      || !plan->guard.owner_current(plan->guard.context)) return false;
  if (!(plan->present&(UINT32_C(1)<<index))) return true;
  if (plan->guard.remove_path) return plan->guard.remove_path(plan->targets[index].path);
  PET_WIDGET_TRACE_SCOPE(cleanup,PET_WTRACE_CLEANUP);
  PET_WIDGET_TRACE_SCOPE(remove_file,PET_WTRACE_FS_REMOVE);
  return remove(plan->targets[index].path)==0 || errno==ENOENT;
}
