/* Private native renderer adapter. Included after codec/pack/scale helpers.
 * Existing priority-1 asset worker, one ASSET_PREPARE lease, one immutable
 * encoded-source pin. No new task, guest service, driver or priority policy.
 * Mux protects descriptors/lifetime only; no codec/broker/alloc/wait in mux. */
#if PET_P4_H264_PREFETCH
static char g_h264_presented_path[80];
static uint64_t g_h264_first_presented_ms;
#if PET_P4_H264_WORKER_PACK
/* Main/render-mutex owner only. The pin exists solely within one render call;
 * unlike QUEUED, this descriptor is never visible to the native worker. */
static struct {
  pet_p4_asset_cache_entry_t *source;
  uint64_t cancel_epoch,storage_epoch;
  char path[80];
  int width,height,frame;
  bool active,pending,restart,flushing;
} g_h264_frame_offer;
static void h264_frame_offer_drop(void){
  portENTER_CRITICAL(&g_asset_prepare_mux);
  if(g_h264_frame_offer.pending&&g_h264_frame_offer.source)
    g_h264_frame_offer.source->decoder_readers--;
  g_h264_frame_offer.pending=false;g_h264_frame_offer.source=NULL;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
}
static void h264_frame_offer_begin(void){
  h264_frame_offer_drop();g_h264_frame_offer.active=true;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  g_h264_frame_offer.cancel_epoch=g_asset_prepare_cancel_epoch;
  g_h264_frame_offer.storage_epoch=g_asset_prepare_storage_epoch;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
}
static bool h264_frame_offer_stage(pet_p4_asset_cache_entry_t *source,const char *path,
  int width,int height,bool restart,int frame){
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool room=!g_h264_frame_offer.pending&&!g_asset_prepare_suspended&&g_asset_prepare_slot<0
    &&g_h264_prefetch_lifetime.phase==PET_PREFETCH_EMPTY&&!g_h264_prefetch_lifetime.cancelled
    &&g_h264_frame_offer.cancel_epoch==g_asset_prepare_cancel_epoch
    &&g_h264_frame_offer.storage_epoch==g_asset_prepare_storage_epoch
    &&(!source||(source->bytes&&source->finished&&!source->failed&&!source->decoder_readers));
  if(room){
    g_h264_frame_offer.source=source;
    g_h264_frame_offer.width=width;g_h264_frame_offer.height=height;
    g_h264_frame_offer.restart=restart;g_h264_frame_offer.frame=frame;
    g_h264_frame_offer.pending=true;if(source)source->decoder_readers++;
  }
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(room)snprintf(g_h264_frame_offer.path,sizeof(g_h264_frame_offer.path),"%s",path?path:"");
  return room;
}
/* Two PSRAM packed-YUV420 buffers (1.5 B/px at the logical frame size). The
 * "front" is g_h264_packed_current.data (render-thread readers only); the
 * "back" is job.packed (worker writer only, WORKING phase only). Static, so no
 * heap, no internal RAM and no allocation on any task. 128 covers the L1 and
 * L2 cache lines, so no line is ever shared with another variable. */
#define PET_P4_H264_WORKER_PACKED_BYTES ((size_t)PET_P4_UI_WIDTH*PET_P4_UI_HEIGHT*3/2)
static EXT_RAM_BSS_ATTR __attribute__((aligned(128))) uint8_t g_h264_worker_packed[2][PET_P4_H264_WORKER_PACKED_BYTES];
static uint8_t *h264_worker_back_buffer(void){
  return g_h264_packed_current.data==g_h264_worker_packed[0]?g_h264_worker_packed[1]:g_h264_worker_packed[0];
}
/* Worker only, inside its WORKING span, after a successful authoritative decode
 * and before any decoder reset or lease finish. Same row kernel and the same
 * half/half split with the existing pack task as the render-thread pack, but
 * no priority boost: the worker never impersonates the renderer. A failure
 * here never fails the job; the render thread falls back to its own pack. */
static void h264_worker_pack(pet_h264_prefetch_job_t *job){
  int stride=0,storage_h=0;job->packed_ok=false;job->pack_us=0;
  if(!job->packed||!job->output||!resolve_h264_i420_layout(job->width,job->height,job->output_size,&stride,&storage_h))return;
  size_t packed_stride=(size_t)job->width*3/2,required=packed_stride*(size_t)storage_h,storage_luma=(size_t)stride*(size_t)storage_h;
  if(storage_h<2||required>job->packed_capacity)return;
  int64_t started_us=esp_timer_get_time();
  pet_p4_h264_pack_job_t primary={.plane_y=job->output,.plane_u=job->output+storage_luma,.plane_v=job->output+storage_luma+storage_luma/4U,
    .packed=job->packed,.packed_stride=packed_stride,.src_w=job->width,.src_stride=stride,.row_begin=0,.row_end=storage_h};
  if(g_h264_pack_task&&g_h264_pack_done){
    /* The pack task is driven from WORKING here and from CONSUMING by the
     * render-thread fallback; the single-job lifetime makes those exclusive. */
    int split_row=storage_h/2;
    g_h264_pack_job=primary;g_h264_pack_job.row_begin=split_row;primary.row_end=split_row;
    while(xSemaphoreTake(g_h264_pack_done,0)==pdTRUE){}
    xTaskNotifyGive(g_h264_pack_task);
    pack_h264_i420_rows(&primary);
    xSemaphoreTake(g_h264_pack_done,portMAX_DELAY);
  }else pack_h264_i420_rows(&primary);
  job->pack_us=elapsed_us_clamped(started_us,esp_timer_get_time());
  job->packed_stride=stride;job->packed_storage_h=storage_h;job->packed_ok=true;
}
/* Render thread at take: adopt the finished back buffer as packed-current when
 * its layout matches the render-side layout. No copy, no pack. */
static bool h264_worker_pack_adopt(const pet_h264_prefetch_job_t *ready,int width,int height,int stride,int storage_h){
  if(!ready->packed||!ready->packed_ok||ready->packed_stride!=stride||ready->packed_storage_h!=storage_h)return false;
  if(!pet_h264_packed_view_init(&g_h264_packed_current,ready->packed,ready->packed_capacity,(size_t)width*3/2,width,height,storage_h))return false;
  pet_render_counter_increment(&g_worker_packs);
  g_worker_pack_last_us=ready->pack_us;
  if(ready->pack_us>g_worker_pack_max_us)g_worker_pack_max_us=ready->pack_us;
  return true;
}
#endif

static void h264_prefetch_cancel_locked(void){
  (void)pet_prefetch_cancel(g_h264_prefetch_job.ticket);
  g_h264_reset_requested=true;
}

static bool h264_prefetch_offer(pet_p4_asset_cache_entry_t *source,const char *path,
  int width,int height,bool restart,int frame){
#if PET_P4_H264_WORKER_PACK
  if(g_h264_frame_offer.active)return h264_frame_offer_stage(source,path,width,height,restart,frame);
#endif
  pet_h264_prefetch_job_t next={.source=source,.width=width,.height=height,
    .restart=restart,.frame=frame,.enqueued_us=(uint64_t)esp_timer_get_time()};
  if(path)snprintf(next.path,sizeof(next.path),"%s",path);
#if PET_P4_H264_WORKER_PACK
  /* Render thread only. The back buffer is never the buffer being displayed,
   * and no worker can hold a buffer here: offers are accepted in EMPTY only. */
  if(source){next.packed=h264_worker_back_buffer();next.packed_capacity=PET_P4_H264_WORKER_PACKED_BYTES;}
#endif
  portENTER_CRITICAL(&g_asset_prepare_mux);
  if(!g_h264_prefetch_lifetime.self)(void)pet_prefetch_init(&g_h264_prefetch_lifetime);
  bool room=g_h264_prefetch_lifetime.phase==PET_PREFETCH_EMPTY
    &&!g_h264_prefetch_lifetime.cancelled&&g_asset_prepare_slot<0&&!g_asset_prepare_suspended;
  next.cancel_epoch=g_asset_prepare_cancel_epoch;
#if PET_P4_H264_WORKER_PACK
  if(g_h264_frame_offer.flushing){
    next.cancel_epoch=g_h264_frame_offer.cancel_epoch;
    room=room&&g_h264_frame_offer.storage_epoch==g_asset_prepare_storage_epoch;
  }
#endif
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  pet_resource_kind_t kind=PET_RESOURCE_ASSET_PREPARE;
  bool audio_blocked=asset_prepare_audio_busy();
#if PET_P4_CONVERSATION_VIDEO
  /* This lease admits only decode of a finished, pinned PSRAM source on the
   * existing single worker. No file/cache allocation is moved into audio. */
  kind=PET_RESOURCE_CACHED_VIDEO; /* NULL-source jobs only close the codec. */
  audio_blocked=audio_blocked&&(!pet_p4_audio_conversation_video_ready()||pet_p4_audio_music_active());
#endif
  if(!room||!g_asset_prepare_task||audio_blocked)return false;
  if(pet_resources_device_owner_open(PET_RESOURCE_MASK(kind),&next.owner)!=PET_RESOURCE_OK)return false;
  if(pet_resources_device_acquire(next.owner,kind,1,&next.lease)!=PET_RESOURCE_OK){
    pet_resources_device_revoke(next.owner);return false;
  }
  bool source_current=!source||pet_resources_device_current(source->buffer_lease);
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool accepted=source_current&&next.cancel_epoch==g_asset_prepare_cancel_epoch
    &&next.cancel_epoch<UINT64_MAX&&!g_asset_prepare_suspended&&g_asset_prepare_slot<0
    &&(!source||(source->bytes&&source->finished&&!source->failed&&!source->decoder_readers))
#if PET_P4_H264_WORKER_PACK
    &&(!g_h264_frame_offer.flushing||g_h264_frame_offer.storage_epoch==g_asset_prepare_storage_epoch)
#endif
    &&pet_prefetch_offer(&g_h264_prefetch_lifetime,next.cancel_epoch+1,&next.ticket);
  if(accepted){
    if(source)source->decoder_readers++;
    g_h264_prefetch_job=next;
  }
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(!accepted){pet_resources_device_revoke(next.owner);(void)pet_resources_device_finish(next.lease);return false;}
  portENTER_CRITICAL(&g_metrics_lock);
  pet_render_counter_increment(&g_metrics.prefetch.jobs);
  portEXIT_CRITICAL(&g_metrics_lock);
  xTaskNotifyGive(g_asset_prepare_task);return true;
}

#if PET_P4_H264_WORKER_PACK
/* Called after the complete frame record, also on every render error path.
 * Cache release/free is render-mutex owned. Keep the pin through validation;
 * cancellation may revoke it but cannot free it. The ordinary offer rechecks
 * epoch under the same mux before publishing QUEUED. */
static void h264_frame_offer_end(bool submit){
  g_h264_frame_offer.active=false;
  if(!g_h264_frame_offer.pending)return;
  pet_p4_asset_cache_entry_t *source=g_h264_frame_offer.source;
  bool authority=!source||pet_resources_device_current(source->buffer_lease);
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool current=submit&&authority&&!g_asset_prepare_suspended
    &&g_h264_frame_offer.cancel_epoch==g_asset_prepare_cancel_epoch
    &&g_h264_frame_offer.storage_epoch==g_asset_prepare_storage_epoch;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  /* Preserve the captured epochs until the final offer check, rather than
   * laundering an old intent through an offer that samples a new epoch. */
  h264_frame_offer_drop();
  if(current){
    int64_t started=esp_timer_get_time();
    g_h264_frame_offer.flushing=true;
    (void)h264_prefetch_offer(source,g_h264_frame_offer.path,g_h264_frame_offer.width,
      g_h264_frame_offer.height,g_h264_frame_offer.restart,g_h264_frame_offer.frame);
    g_h264_frame_offer.flushing=false;
    uint32_t elapsed=elapsed_us_clamped(started,esp_timer_get_time());
    portENTER_CRITICAL(&g_metrics_lock);
    g_metrics.prefetch.last_offer_us=elapsed;
    if(elapsed>g_metrics.prefetch.max_offer_us)g_metrics.prefetch.max_offer_us=elapsed;
    portEXIT_CRITICAL(&g_metrics_lock);
  }
}
#endif

/* Main only. Never touches codec memory while a worker/consumer is active.
 * Returning false means retirement/cleanup pending, NOT decoder failure. */
static bool h264_prefetch_reset(void){
  g_h264_packed_current=(pet_h264_packed_view_t){0};g_h264_presented_frame=-1;
  g_h264_first_presented_ms=0;g_h264_presented_path[0]=0;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  h264_prefetch_cancel_locked();
  pet_resource_owner_t owner=g_h264_prefetch_job.owner;
  bool retired=!g_h264_prefetch_lifetime.self||pet_prefetch_retire(&g_h264_prefetch_lifetime);
  if(retired)g_h264_prefetch_job=(pet_h264_prefetch_job_t){0};
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  pet_resources_device_revoke(owner);
  if(!retired)return false;
  /* No worker can start without an explicit offer from this main parent. */
  if(g_h264_decoder){(void)h264_prefetch_offer(NULL,NULL,0,0,true,-1);return false;}
  portENTER_CRITICAL(&g_asset_prepare_mux);
  g_h264_reset_requested=false;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  return true;
}

static bool h264_prefetch_worker(void){
  pet_h264_prefetch_job_t job;pet_p4_asset_cache_entry_t source={0};bool execute=false;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool claimed=!g_h264_program_window&&pet_prefetch_claim(g_h264_prefetch_job.ticket,&execute);
  if(claimed){job=g_h264_prefetch_job;if(job.source)source=*job.source;}
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(!claimed)return false;
  int64_t started_us=esp_timer_get_time();
  source.owner=job.owner;source.prepare_lease=job.lease;
  pet_p4_render_trace_t trace={0}; /* Job-local, never the current main frame. */
  bool ok=execute&&job.source&&asset_prepare_current(&source);
  if(ok&&job.restart)ok=start_h264_decoder_role(job.path,source.bytes,source.size,&trace,&source);
  if(ok)ok=decode_next_h264_frame_role(job.width,job.height,false,&job.decode_us,&source);
  bool authority=job.source&&asset_prepare_current(&source);
  ok=ok&&authority;
  if(ok){job.output=g_h264_output;job.output_size=g_h264_output_size;ok=g_h264_decoded_frame==job.frame;}
#if PET_P4_H264_WORKER_PACK
  /* Still inside WORKING with the decoder and the pinned source alive. The
   * pack widens the window after the authority check, so re-check it: a
   * cancellation that landed inside the pack closes the codec now, exactly
   * like one that landed inside the decode. */
  if(ok){h264_worker_pack(&job);ok=job.source&&asset_prepare_current(&source);}
#endif
  /* Discarded or cleanup-only work closes only after SDK returned. A final
   * concurrent cancellation may leave this codec allocated until next cleanup,
   * but can never deliver/reuse its output or release a physical reader early. */
  if(!ok)reset_h264_decoder();
  bool lease_finished_current=pet_resources_device_finish(job.lease)==PET_RESOURCE_FINISH_CURRENT;
  ok=lease_finished_current&&ok;
  pet_resources_device_revoke(job.owner);
  uint32_t free_bytes=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
  job.wall_us=elapsed_us_clamped(started_us,esp_timer_get_time());
  portENTER_CRITICAL(&g_asset_prepare_mux);
  if(job.source&&job.source->decoder_readers)job.source->decoder_readers--;
  job.success=ok;
  g_h264_prefetch_job=job;
  if(!execute||!authority||!lease_finished_current){
    (void)pet_prefetch_cancel(job.ticket);g_h264_reset_requested=true;
  }
  (void)pet_prefetch_complete(job.ticket,ok);
  bool delivered=g_h264_prefetch_lifetime.phase==PET_PREFETCH_READY;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  /* Publication permits descriptor reuse: every value below is job-local. */
  portENTER_CRITICAL(&g_metrics_lock);
  pet_render_counter_increment(&g_metrics.prefetch.completed);
  if(!delivered)pet_render_counter_increment(&g_metrics.prefetch.cancelled);
  if(ok)pet_render_counter_increment(&g_metrics.prefetch.decoded_frames);
  g_metrics.prefetch.last_worker_us=job.wall_us;g_metrics.prefetch.last_decode_us=job.decode_us;
  if(job.wall_us>g_metrics.prefetch.max_worker_us)g_metrics.prefetch.max_worker_us=job.wall_us;
  if(job.decode_us>g_metrics.prefetch.max_decode_us)g_metrics.prefetch.max_decode_us=job.decode_us;
  if(!g_metrics.prefetch.min_worker_free_bytes||free_bytes<g_metrics.prefetch.min_worker_free_bytes)g_metrics.prefetch.min_worker_free_bytes=free_bytes;
  portEXIT_CRITICAL(&g_metrics_lock);
  return true;
}

static void scale_h264_packed_cpu(const pet_h264_packed_view_t *v){
  int x,y,w,h;
  if(!v->data||!asset_fit_layout(v->width,v->height,&x,&y,&w,&h,NULL))return;
  memset(g_framebuffer,0,PET_P4_UI_WIDTH*PET_P4_UI_HEIGHT*sizeof(uint16_t));
  for(int dy=0;dy<h;dy++){
    int sy=dy*v->height/h;uint16_t *row=g_framebuffer+(y+dy)*PET_P4_UI_WIDTH+x;
    for(int dx=0;dx<w;dx++)row[dx]=pet_h264_packed_pixel(v,dx*v->width/w,sy);
  }
}

static bool render_h264_asset_frame_async(const pet_p4_asset_selection_t *asset,const char *path,
  pet_p4_asset_cache_entry_t *source,uint64_t duration_ms){
  if(!asset||!source||!source->single_slice_valid||!duration_ms||asset->frames<=0||asset->width<=0||asset->height<=0)return false;
  g_asset_cache_pending=false;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool reset=g_h264_reset_requested;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(reset||strcmp(path,g_h264_presented_path)){
    if(!h264_prefetch_reset()){g_asset_cache_pending=true;return false;}
    snprintf(g_h264_presented_path,sizeof(g_h264_presented_path),"%s",path);
  }
  uint64_t now_ms=renderer_monotonic_ms();
  uint64_t elapsed=g_h264_first_presented_ms&&now_ms>=g_h264_first_presented_ms?now_ms-g_h264_first_presented_ms:0;
  int target=(int)((elapsed*(uint64_t)asset->frames)/duration_ms);
  if(target>=asset->frames)target=asset->frames-1;
  if(elapsed>=duration_ms&&g_h264_presented_frame==asset->frames-1){
    (void)h264_prefetch_reset();g_asset_cache_pending=true;return false;
  }
  pet_h264_prefetch_job_t ready={0};bool taken=false,failed=false;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  pet_prefetch_phase_t phase=g_h264_prefetch_lifetime.phase;
  if(phase==PET_PREFETCH_DISCARDED){
    failed=!g_h264_prefetch_lifetime.cancelled&&g_h264_prefetch_job.source;
    (void)pet_prefetch_retire(&g_h264_prefetch_lifetime);
    g_h264_prefetch_job=(pet_h264_prefetch_job_t){0};
  }else if(phase==PET_PREFETCH_READY&&g_h264_prefetch_job.frame<=target
      &&!strcmp(g_h264_prefetch_job.path,path)){
    ready=g_h264_prefetch_job;taken=pet_prefetch_take(ready.ticket,ready.cancel_epoch+1);
  }
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(failed){portENTER_CRITICAL(&g_asset_prepare_mux);g_h264_reset_requested=true;portEXIT_CRITICAL(&g_asset_prepare_mux);return false;}
  bool direct_cpu=false;
  if(taken){
    int stride=0,storage_h=0;const uint8_t *packed=NULL;
    bool layout=resolve_h264_i420_layout(asset->width,asset->height,ready.output_size,&stride,&storage_h);
    int64_t pack_started=esp_timer_get_time();
#if PET_P4_H264_WORKER_PACK
    bool packed_ok=layout&&h264_worker_pack_adopt(&ready,asset->width,asset->height,stride,storage_h);
    if(layout&&!packed_ok){
      pet_render_counter_increment(&g_worker_pack_fallbacks);
      packed_ok=pack_h264_i420_for_ppa(ready.output,asset->width,asset->height,stride,storage_h,&packed)
        &&pet_h264_packed_view_init(&g_h264_packed_current,packed,g_h264_ppa_input_capacity,
          (size_t)asset->width*3/2,asset->width,asset->height,storage_h);
    }
#else
    bool packed_ok=layout&&pack_h264_i420_for_ppa(ready.output,asset->width,asset->height,stride,storage_h,&packed)
      &&pet_h264_packed_view_init(&g_h264_packed_current,packed,g_h264_ppa_input_capacity,
        (size_t)asset->width*3/2,asset->width,asset->height,storage_h);
#endif
    g_last_h264_pack_us+=elapsed_us_clamped(pack_started,esp_timer_get_time());
    if(layout&&!packed_ok){
      g_h264_packed_current=(pet_h264_packed_view_t){0};
      g_render_trace.fallback|=PET_RENDER_FALLBACK_H264_CPU;
      scale_h264_frame_cpu(ready.output,asset->width,asset->height,stride,storage_h);direct_cpu=true;
    }
    /* pack_h264_i420_for_ppa has joined its existing pack worker; no output
     * reader remains before release and the next native decode offer. */
    portENTER_CRITICAL(&g_asset_prepare_mux);
    bool current=!g_h264_prefetch_lifetime.cancelled;
    (void)pet_prefetch_release(ready.ticket);
    portEXIT_CRITICAL(&g_asset_prepare_mux);
    if(!layout||!current){
      portENTER_CRITICAL(&g_asset_prepare_mux);g_h264_reset_requested=true;portEXIT_CRITICAL(&g_asset_prepare_mux);
      g_asset_cache_pending=!current;return false;
    }
    g_h264_presented_frame=ready.frame;
    if(!g_h264_first_presented_ms)g_h264_first_presented_ms=now_ms;
    uint32_t latency=elapsed_us_clamped((int64_t)ready.enqueued_us,esp_timer_get_time());
    portENTER_CRITICAL(&g_metrics_lock);
    pet_render_counter_increment(&g_metrics.prefetch.presented_frames);
    if(latency>g_metrics.prefetch.max_ready_latency_us)g_metrics.prefetch.max_ready_latency_us=latency;
    portEXIT_CRITICAL(&g_metrics_lock);
  }
  /* Repeated display reads ONLY independently packed-current pixels. A ready
   * future frame remains held until its declared target; never shown early. */
  if(g_h264_presented_frame<asset->frames-1){
    (void)h264_prefetch_offer(source,path,asset->width,asset->height,g_h264_presented_frame<0,g_h264_presented_frame+1);
  }
  if(direct_cpu)return true;
  if(!g_h264_packed_current.data){g_asset_cache_pending=true;return false;}
  int64_t scale_started=esp_timer_get_time();
  if(!h264_frame_supports_ppa_yuv420(asset->width,g_h264_packed_current.storage_height)
      ||!scale_h264_frame_ppa(g_h264_packed_current.data,asset->width,asset->height,g_h264_packed_current.storage_height)){
    g_render_trace.fallback|=PET_RENDER_FALLBACK_H264_CPU;scale_h264_packed_cpu(&g_h264_packed_current);
  }
  pet_p4_render_trace_add(&g_render_trace.scale_us,elapsed_us_clamped(scale_started,esp_timer_get_time()));
  return true;
}
#endif
