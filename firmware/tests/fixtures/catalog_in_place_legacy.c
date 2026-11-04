/* Historical writer for the runtime negative control. No device or private data.
 * It can acknowledge a later cold-corrupt generation after a short cross-page
 * shrink/grow on the current SDK. This file is compiled only by the host test. */
static bool write_catalog_in_place(const char *path,const char *data,size_t len,bool *may_have_changed){
  if(!storage_work_current()||!path||!data)return false;
  PET_WIDGET_TRACE_SCOPE(trace_open, PET_WTRACE_FS_OPEN);
  FILE *file=fopen(path,"r+b");
  if(!file&&errno==ENOENT)file=fopen(path,"wb");
  pet_widget_trace_scope_end(&trace_open);
  if(!file){snprintf(g_storage_failure,sizeof(g_storage_failure),"open %s errno=%d",path,errno);return false;}
  PET_WIDGET_TRACE_SCOPE(trace_data, PET_WTRACE_FS_DATA);
  bool ok=false;
  if(storage_work_current()){
    *may_have_changed=true; /* A short/error write may still seal a valid prefix. */
    ok=fwrite(data,1,len,file)==len&&fflush(file)==0;
  }
  if(ok)ok=storage_work_current()&&ftruncate(fileno(file),(off_t)len)==0;
  int saved_errno=errno;
  pet_widget_trace_scope_end(&trace_data);
  PET_WIDGET_TRACE_SCOPE(trace_close, PET_WTRACE_FS_CLOSE);
  if(fclose(file)!=0){ok=false;saved_errno=errno;}
  pet_widget_trace_scope_end(&trace_close);
  vTaskDelay(pdMS_TO_TICKS(1));
  if(!ok)snprintf(g_storage_failure,sizeof(g_storage_failure),"write/close %s errno=%d bytes=%u",path,saved_errno,(unsigned)len);
  return ok&&storage_work_current();
}
