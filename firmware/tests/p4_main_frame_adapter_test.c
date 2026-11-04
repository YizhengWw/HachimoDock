/* R145 worker-pack variant of the deterministic native-adapter harness: the
 * ACTUAL prefetch include runs with PET_P4_H264_WORKER_PACK=1. Native decoder
 * and RTOS are seams (the pack task runs synchronously inside xTaskNotifyGive);
 * resource broker, lifetime and production admission are real. */
#include "pet_p4_prefetch_lifetime.h"
#include "pet_p4_h264_packed.h"
#include "pet_p4_h264_pack_word.h"
#include "pet_p4_render_trace.h"
#include "pet_p4_render_lifetime.h"
#include "pet_p4_task_policy.h"
#include "pet_p4_resources.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !PET_P4_H264_WORKER_PACK || !PET_P4_H264_PREFETCH
#error this harness exercises the worker-pack path
#endif
#define EXT_RAM_BSS_ATTR
#define PET_P4_ASSET_CACHE_SLOTS 2
#define PET_P4_ASSET_PREPARE_CHUNK_BYTES 16384U
#define PET_P4_ASSET_CACHE_MAX_FILE_BYTES (2U*1024U*1024U)
#define PET_P4_H264_PRIMARY_TASK_PRIORITY PET_P4_H264_TASK_PRIORITY
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define ESP_TASK_MAIN_PRIO 1U
#define ESP_H264_RAW_FMT_I420 0
#define ESP_H264_ERR_OK 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGW(...) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdFALSE 0
typedef int portMUX_TYPE;typedef unsigned char StackType_t;typedef unsigned UBaseType_t;typedef int TaskHandle_t;typedef void *SemaphoreHandle_t;
typedef void *esp_h264_dec_handle_t;typedef int esp_h264_err_t;
typedef struct {int pic_type;}esp_h264_dec_cfg_sw_t;
typedef struct {struct{uint8_t *buffer;uint32_t len;}raw_data;uint32_t consume;}esp_h264_dec_in_frame_t;
typedef struct {uint8_t *outbuf;uint32_t out_size;}esp_h264_dec_out_frame_t;
typedef struct {int frames,width,height;}pet_p4_asset_selection_t;
typedef struct {const uint8_t *plane_y,*plane_u,*plane_v;uint8_t *packed;size_t packed_stride;int src_w,src_stride,row_begin,row_end;}pet_p4_h264_pack_job_t;
static unsigned critical,notifications,codec_calls,codec_frees,freed_buffers,render_packs,worker_pack_rows,split_packs;
static unsigned hook;static bool fail_new,fail_decode,fail_worker_layout,playback_busy,pack_given;
static uint64_t now_us=100000;static pet_resource_broker_t broker;
static uint8_t decoded[16*16*3/2],packed_pixels[sizeof(decoded)];
static uint16_t framebuffer[640*480],*g_framebuffer=framebuffer,expected_pixel;
static pet_p4_render_trace_t g_render_trace;static uint32_t g_last_h264_pack_us;
static int g_metrics_lock;
static struct {struct {uint32_t jobs,completed,cancelled,decoded_frames,presented_frames,last_worker_us,max_worker_us,last_decode_us,max_decode_us,max_ready_latency_us,min_worker_free_bytes,last_offer_us,max_offer_us;}prefetch;}g_metrics;
static uint32_t g_worker_packs,g_worker_pack_fallbacks,g_worker_pack_max_us,g_worker_pack_last_us;
static TaskHandle_t g_h264_pack_task;static SemaphoreHandle_t g_h264_pack_done;static pet_p4_h264_pack_job_t g_h264_pack_job;
static uint8_t fill_of[2];
static void enter(void){assert(!critical);critical++;}
static void leave(void){assert(critical==1);critical--;}
#define portENTER_CRITICAL(m) ((void)(m),enter())
#define portEXIT_CRITICAL(m) ((void)(m),leave())
static void actual_pack_rows(const pet_p4_h264_pack_job_t *job);
static void pack_h264_i420_rows(const pet_p4_h264_pack_job_t *job);
static void interleave(unsigned at);
/* The pack task's half runs synchronously here, through the same counted kernel. */
static void xTaskNotifyGive(TaskHandle_t task){
 assert(task&&!critical);notifications++;
 if(task==2){assert(!pack_given);pack_h264_i420_rows(&g_h264_pack_job);pack_given=true;split_packs++;}
}
static int xSemaphoreTake(SemaphoreHandle_t sem,unsigned timeout){
 assert(sem==g_h264_pack_done&&!critical);
 if(pack_given){pack_given=false;return pdTRUE;}
 assert(timeout==0&&"pack join without a running pack task would deadlock");return pdFALSE;
}
static int64_t esp_timer_get_time(void){return (int64_t)now_us;}
static uint64_t renderer_monotonic_ms(void){return now_us/1000;}
static uint32_t elapsed_us_clamped(int64_t from,int64_t to){return to>=from?(uint32_t)(to-from):0;}
static uint32_t uxTaskGetStackHighWaterMark(void *task){(void)task;assert(!critical);return 1234;}
static UBaseType_t uxTaskPriorityGet(void *task){(void)task;return 1;}
static void vTaskPrioritySet(void *task,UBaseType_t priority){(void)task;(void)priority;assert(!"worker must not impersonate renderer");}
static bool pet_p4_audio_active(void){return playback_busy;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
static pet_resource_result_t pet_resources_device_owner_open(uint32_t mask,pet_resource_owner_t *out){assert(!critical);return pet_resources_owner_open(&broker,mask,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out){assert(!critical);return pet_resources_try_acquire(&broker,owner,kind,units,out);}
static bool pet_resources_device_current(pet_resource_lease_t lease){assert(!critical);return pet_resources_current(&broker,lease);}
static void pet_resources_device_revoke(pet_resource_owner_t owner){assert(!critical);(void)pet_resources_owner_revoke(&broker,owner);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t lease){assert(!critical);return pet_resources_finish(&broker,lease);}
static void *heap_caps_realloc(void *ptr,size_t size,int caps){assert(!critical&&caps==3);return realloc(ptr,size);}
static void heap_caps_free(void *ptr){assert(!critical);freed_buffers++;free(ptr);}
static int esp_h264_dec_close(void *handle){assert(handle&&!critical);return 0;}
static int esp_h264_dec_del(void *handle){assert(handle&&!critical);codec_frees++;return 0;}
static int esp_h264_dec_sw_new(const esp_h264_dec_cfg_sw_t *cfg,void **out){assert(!critical&&cfg->pic_type==0);*out=fail_new?NULL:decoded;return fail_new?-1:0;}
static int esp_h264_dec_open(void *handle){assert(handle&&!critical);return 0;}
static int esp_h264_dec_process(void *handle,esp_h264_dec_in_frame_t *in,esp_h264_dec_out_frame_t *out){
 assert(handle&&!critical);codec_calls++;interleave(1);
 if(fail_decode)return -1;
 assert(in->raw_data.len);in->raw_data.buffer[0]=0xa5;in->consume=1;
 memset(decoded,30+(int)codec_calls,sizeof(decoded));
 out->outbuf=decoded;out->out_size=sizeof(decoded);now_us+=3000;return 0;
}
/* ACTUAL_DECLARATIONS */
/* ACTUAL_FUNCTIONS */
static bool resolve_h264_i420_layout(int w,int h,uint32_t size,int *stride,int *height){
 assert(w==16&&h==16);*stride=16;*height=16;
 if(fail_worker_layout&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_WORKING)return false;
 return size==sizeof(decoded);
}
static int buffer_index(const uint8_t *p);
static void pack_h264_i420_rows(const pet_p4_h264_pack_job_t *job){
 assert(!critical&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_WORKING);
 interleave(4);actual_pack_rows(job);worker_pack_rows+=(unsigned)(job->row_end-job->row_begin);
 int index=buffer_index(job->packed);assert(index>=0);fill_of[index]=job->plane_y[0];
}
static bool pack_h264_i420_for_ppa(const uint8_t *in,int w,int h,int stride,int height,const uint8_t **out){
 assert(w==16&&h==16&&stride==16&&height==16&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_CONSUMING);render_packs++;
 for(int y=0;y<h;y++)for(int x=0;x<w;x+=2){
   size_t at=(size_t)y*24+(x/2)*3;packed_pixels[at]=in[256+((y&1)?64:0)+(y/2)*8+x/2];
   packed_pixels[at+1]=in[y*16+x];packed_pixels[at+2]=in[y*16+x+1];
 }
 g_h264_ppa_input=packed_pixels;g_h264_ppa_input_capacity=sizeof(packed_pixels);*out=packed_pixels;return true;
}
static bool asset_fit_layout(int w,int h,int *x,int *y,int *dw,int *dh,float *scale){(void)scale;assert(w==16&&h==16);*x=*y=0;*dw=*dh=16;return true;}
static void scale_h264_frame_cpu(const uint8_t *in,int w,int h,int stride,int sh){
 (void)in;(void)w;(void)h;(void)stride;(void)sh;assert(!"worker-packed frames never read I420 on the render thread");
}
static bool h264_frame_supports_ppa_yuv420(int w,int h){return !(w%16)&&!(h%16);}
static bool scale_h264_frame_ppa(const uint8_t *in,int w,int h,int storage_h){
 assert(w==16&&h==16&&storage_h==16&&in==g_h264_packed_current.data);
 int index=buffer_index(in);
 if(index>=0)for(size_t i=0;i<16*24;i++)assert(in[i]==fill_of[index]);
 else assert(in==packed_pixels);
 uint8_t before[16*24];memcpy(before,in,sizeof(before));
 pet_h264_packed_view_t view;assert(pet_h264_packed_view_init(&view,in,16*24,24,16,16,16));
 expected_pixel=pet_h264_packed_pixel(&view,0,0);
 /* The next job cannot be claimed during current-frame conversion. */
 assert(!h264_prefetch_worker());assert(!memcmp(before,in,sizeof(before)));return false;
}
#include "pet_p4_h264_prefetch.inc"
static int buffer_index(const uint8_t *p){
 if(p>=g_h264_worker_packed[0]&&p<g_h264_worker_packed[0]+PET_P4_H264_WORKER_PACKED_BYTES)return 0;
 if(p>=g_h264_worker_packed[1]&&p<g_h264_worker_packed[1]+PET_P4_H264_WORKER_PACKED_BYTES)return 1;
 return -1;
}
static void interleave(unsigned at){
 if(hook==at){hook=0;assert(!pet_p4_renderer_program_window_begin());
   unsigned frees=codec_frees;assert(!h264_prefetch_reset());assert(codec_frees==frees);
 }
 if(hook==10+at){hook=0;unsigned freed=freed_buffers;
   assert(g_asset_cache[0].decoder_readers==1);asset_cache_release(&g_asset_cache[0]);
   assert(g_asset_cache[0].bytes&&freed_buffers==freed);
 }
}
static void fresh_source(void){
 for(unsigned i=0;i<10&&!h264_prefetch_reset();i++)(void)h264_prefetch_worker();
 assert(!g_h264_decoder&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_EMPTY&&!g_h264_packed_current.data);
 asset_cache_release(&g_asset_cache[0]);pet_p4_asset_cache_entry_t *s=&g_asset_cache[0];
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_ASSET_BUFFER),&s->owner)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(s->owner,PET_RESOURCE_ASSET_BUFFER,32,&s->buffer_lease)==PET_RESOURCE_OK);
 s->bytes=malloc(32);assert(s->bytes);memset(s->bytes,0x19,32);s->size=32;s->finished=true;s->h264=true;s->single_slice_valid=true;
}
/* Model the actual render-call boundary; native work from the previous
 * tail may finish between frames. Never execute new work during conversion. */
static bool harness_frame(const pet_p4_asset_selection_t *asset,const char *path,
 pet_p4_asset_cache_entry_t *source,uint64_t duration){
 (void)h264_prefetch_worker();
 h264_frame_offer_begin();
 bool rendered=render_h264_asset_frame_async(asset,path,source,duration);
 assert(g_h264_prefetch_lifetime.phase!=PET_PREFETCH_QUEUED);
 h264_frame_offer_end(true);
 assert(!g_h264_frame_offer.active&&!g_h264_frame_offer.pending&&!g_h264_frame_offer.source);
 return rendered;
}
int main(void){
 _Static_assert(sizeof(g_h264_worker_packed)==2*640*480*3/2,"two logical-frame packed buffers");
 assert(((uintptr_t)g_h264_worker_packed[0]&127U)==0&&((uintptr_t)g_h264_worker_packed[1]&127U)==0);
 g_asset_prepare_task=1;g_h264_pack_task=2;g_h264_pack_done=(void*)&g_h264_pack_done;
 pet_p4_asset_selection_t asset={3,16,16};fresh_source();
 /* Offer carries the back buffer; a cleanup-only offer carries none. */
 assert(!harness_frame(&asset,"a",&g_asset_cache[0],300)&&g_asset_cache_pending);
 assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_QUEUED&&g_asset_cache[0].decoder_readers==1);
 assert(g_h264_prefetch_job.packed==g_h264_worker_packed[0]&&g_h264_prefetch_job.packed_capacity==PET_P4_H264_WORKER_PACKED_BYTES);
 assert(pet_p4_renderer_program_window_begin());assert(!pet_p4_renderer_program_window_begin());
 assert(!h264_prefetch_worker()&&!codec_calls);pet_p4_renderer_program_window_end();assert(h264_prefetch_worker());
 assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_READY&&!g_asset_cache[0].decoder_readers);
 assert(g_h264_prefetch_job.packed_ok&&g_h264_prefetch_job.packed_stride==16&&g_h264_prefetch_job.packed_storage_h==16&&worker_pack_rows==16&&split_packs==1);
 /* Take adopts buffer 0 without any render-thread pack; the next offer gets buffer 1. */
 assert(harness_frame(&asset,"a",&g_asset_cache[0],300));
 assert(g_h264_presented_frame==0&&framebuffer[0]==expected_pixel&&g_h264_prefetch_job.frame==1);
 assert(g_h264_packed_current.data==g_h264_worker_packed[0]&&g_h264_prefetch_job.packed==g_h264_worker_packed[1]);
 assert(!render_packs&&g_worker_packs==1&&!g_worker_pack_fallbacks&&!g_last_h264_pack_us);
 assert(harness_frame(&asset,"a",&g_asset_cache[0],300));assert(g_h264_presented_frame==0); /* no early next-frame display */
 assert(g_h264_packed_current.data==g_h264_worker_packed[0]);
 now_us=(g_h264_first_presented_ms+100)*1000;assert(harness_frame(&asset,"a",&g_asset_cache[0],300));assert(g_h264_presented_frame==1);
 assert(g_h264_packed_current.data==g_h264_worker_packed[1]&&g_h264_prefetch_job.packed==g_h264_worker_packed[0]);
 now_us=(g_h264_first_presented_ms+300)*1000;assert(harness_frame(&asset,"a",&g_asset_cache[0],300));assert(g_h264_presented_frame==2);
 assert(g_h264_packed_current.data==g_h264_worker_packed[0]&&framebuffer[0]==expected_pixel);
 assert(g_metrics.prefetch.presented_frames==3&&g_metrics.prefetch.decoded_frames==3&&g_worker_packs==3&&!render_packs);
 now_us=(g_h264_first_presented_ms+301)*1000;assert(!harness_frame(&asset,"a",&g_asset_cache[0],300)); /* tail first, loop cleanup next */
 (void)h264_prefetch_worker();fresh_source();
 for(unsigned scenario=0;scenario<6;scenario++){
   unsigned frees=codec_frees,calls=codec_calls,rows=worker_pack_rows;assert(h264_prefetch_offer(&g_asset_cache[0],"cancel",16,16,true,0));
   if(scenario==0){pet_p4_renderer_cancel_asset_preparation();assert(!g_h264_decoder);}
   if(scenario==1)hook=1; /* cancellation physically inside SDK */
   if(scenario==2)hook=11; /* storage revokes exact still-pinned source */
   if(scenario==3)fail_new=true;
   if(scenario==4)fail_decode=true;
   if(scenario==5)hook=4; /* cancellation physically inside the worker pack */
   assert(h264_prefetch_worker());assert(!g_asset_cache[0].decoder_readers);
   assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_DISCARDED&&!g_h264_decoder);
   assert(codec_calls==calls+(scenario==0||scenario==3?0:1));assert(codec_frees>=frees);
   assert(worker_pack_rows==rows+(scenario==5?16:0)); /* only an authoritative decode is packed */
   fail_new=fail_decode=false;fresh_source();
 }
 /* A ready result cannot survive audio authority revocation or be replayed. */
 assert(h264_prefetch_offer(&g_asset_cache[0],"audio",16,16,true,0));
 pet_resource_realtime_t audio={0};assert(pet_resources_realtime_begin(&broker,&audio)==PET_RESOURCE_OK);
 assert(h264_prefetch_worker());assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_DISCARDED);
 assert(pet_resources_realtime_finish(&broker,audio));fresh_source();
 playback_busy=true;assert(!h264_prefetch_offer(&g_asset_cache[0],"busy",16,16,true,0));playback_busy=false;
 /* Worker-side layout failure: the job stays READY, the render thread keeps its
  * original pack as the counted fallback, and the next offer avoids the
  * fallback buffer's front. */
 assert(!harness_frame(&asset,"fallback",&g_asset_cache[0],300));
 fail_worker_layout=true;assert(h264_prefetch_worker());fail_worker_layout=false;
 assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_READY&&!g_h264_prefetch_job.packed_ok);
 assert(harness_frame(&asset,"fallback",&g_asset_cache[0],300));
 assert(g_h264_presented_frame==0&&render_packs==1&&g_worker_pack_fallbacks==1&&g_worker_packs==3);
 assert(g_h264_packed_current.data==packed_pixels&&g_h264_prefetch_job.packed==g_h264_worker_packed[0]);
 /* The tail queued frame 1 only after frame 0 conversion; run it between
  * calls into buffer 0, never into the displayed fallback front. */
 assert(h264_prefetch_worker()&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_READY&&g_h264_prefetch_job.packed_ok);
 now_us=(g_h264_first_presented_ms+100)*1000;
 assert(harness_frame(&asset,"fallback",&g_asset_cache[0],300));
 assert(g_h264_presented_frame==1&&g_h264_packed_current.data==g_h264_worker_packed[0]&&g_worker_packs==4&&render_packs==1);
 assert(g_worker_pack_last_us==0&&g_worker_pack_max_us==0); /* deterministic clock: pack wall 0 */
 fresh_source();
 /* Without the pack task the worker packs every row itself. */
 g_h264_pack_task=0;g_h264_pack_done=NULL;unsigned splits=split_packs,rows=worker_pack_rows;
 assert(!harness_frame(&asset,"single",&g_asset_cache[0],300));assert(h264_prefetch_worker());
 assert(split_packs==splits&&worker_pack_rows==rows+16&&g_h264_prefetch_job.packed_ok);
 assert(harness_frame(&asset,"single",&g_asset_cache[0],300)&&g_h264_packed_current.data==g_h264_worker_packed[0]);
 g_h264_pack_task=2;g_h264_pack_done=(void*)&g_h264_pack_done;fresh_source();
 /* Other existing filesystem work owns the same single preparation quota. */
 pet_resource_owner_t io={0};pet_resource_lease_t io_lease={0};
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE),&io)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(io,PET_RESOURCE_ASSET_PREPARE,1,&io_lease)==PET_RESOURCE_OK);
 assert(!h264_prefetch_offer(&g_asset_cache[0],"occupied",16,16,true,0));
 assert(pet_resources_device_finish(io_lease)==PET_RESOURCE_FINISH_CURRENT);pet_resources_device_revoke(io);
 /* Full physical retirement and balanced broker occupancy after every branch. */
 fresh_source();asset_cache_release(&g_asset_cache[0]);pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);
 assert(!usage.live_owners&&!usage.revoked_owners&&!usage.leases[PET_RESOURCE_ASSET_BUFFER]&&!usage.leases[PET_RESOURCE_ASSET_PREPARE]);
 assert(g_metrics.prefetch.min_worker_free_bytes==1234&&notifications&&!pack_given);
 free(g_h264_work_stream);return 0;
}
