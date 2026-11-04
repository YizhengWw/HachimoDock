"""Production deferred-offer functions, real broker/lifetime, deterministic RTOS seams."""
from pathlib import Path
from p4_render_trace_test import function
from p4_h264_prefetch_test import compile_run
ROOT=Path(__file__).resolve().parents[1]

def test_actual_frame_offer_never_publishes_until_tail_and_rejects_revoked_intents(tmp_path):
    text=(ROOT/'main/pet_p4_h264_prefetch.inc').read_text()
    descriptor=text[text.index('static struct {'):text.index('static void h264_frame_offer_drop')]
    release=function((ROOT/'main/pet_p4_renderer.c').read_text(),'asset_cache_release')
    funcs='\n'.join(function(text,n) for n in ('h264_frame_offer_drop','h264_frame_offer_begin','h264_frame_offer_stage','h264_prefetch_offer','h264_frame_offer_end'))
    code=r'''
#include "pet_p4_prefetch_lifetime.h"
#include "pet_p4_resources.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_P4_H264_WORKER_PACK 1
#define PET_P4_H264_PREFETCH 1
#define PET_P4_H264_WORKER_PACKED_BYTES 384
#define portENTER_CRITICAL(m) ((void)(m),enter())
#define portEXIT_CRITICAL(m) ((void)(m),leave())
typedef struct {uint8_t *bytes;bool finished,failed;unsigned decoder_readers;pet_resource_lease_t buffer_lease,prepare_lease;pet_resource_owner_t owner;bool loading;}pet_p4_asset_cache_entry_t;
typedef struct {pet_p4_asset_cache_entry_t *source;int width,height,frame;bool restart;uint64_t enqueued_us,cancel_epoch;char path[80];uint8_t *packed;size_t packed_capacity;pet_resource_owner_t owner;pet_resource_lease_t lease;pet_prefetch_ticket_t ticket;}pet_h264_prefetch_job_t;
static unsigned depth,notifications,hook,freed;static bool suspended_audio;
static int g_asset_prepare_mux,g_metrics_lock,g_asset_prepare_slot=-1,g_asset_prepare_task=1;
static uint64_t g_asset_prepare_cancel_epoch,g_asset_prepare_storage_epoch;
static bool g_asset_prepare_suspended;
static pet_prefetch_lifetime_t g_h264_prefetch_lifetime;
static pet_h264_prefetch_job_t g_h264_prefetch_job;
static pet_resource_broker_t broker;
static struct {struct {uint32_t jobs,last_offer_us,max_offer_us;}prefetch;}g_metrics;
static uint8_t pixels[384];
static void enter(void){assert(!depth);depth++;}
static void leave(void){assert(depth==1);depth--;}
static uint64_t esp_timer_get_time(void){return 123456;}
static uint32_t elapsed_us_clamped(int64_t a,int64_t b){return (uint32_t)(b-a);}
static void pet_render_counter_increment(uint32_t *v){(*v)++;}
static uint8_t *h264_worker_back_buffer(void){return pixels;}
static bool asset_prepare_audio_busy(void){return suspended_audio;}
static void xTaskNotifyGive(int task){assert(task&&!depth);notifications++;}
static pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t *o){
 assert(!depth);if(hook==1){hook=0;g_asset_prepare_cancel_epoch++;}if(hook==2){hook=0;g_asset_prepare_storage_epoch++;}
 return pet_resources_owner_open(&broker,m,o);
}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t n,pet_resource_lease_t *l){assert(!depth);return pet_resources_try_acquire(&broker,o,k,n,l);}
static bool pet_resources_device_current(pet_resource_lease_t l){assert(!depth);return pet_resources_current(&broker,l);}
static void pet_resources_device_revoke(pet_resource_owner_t o){assert(!depth);pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){assert(!depth);return pet_resources_finish(&broker,l);}
static void heap_caps_free(void *p){assert(p&&!depth);freed++;}
'''+descriptor+release+funcs+r'''
static void retire(void){
 if(g_h264_prefetch_lifetime.phase==PET_PREFETCH_QUEUED){
  bool run=false;assert(pet_prefetch_claim(g_h264_prefetch_job.ticket,&run));assert(run);
  assert(pet_prefetch_complete(g_h264_prefetch_job.ticket,true));
 }
 if(g_h264_prefetch_lifetime.phase==PET_PREFETCH_READY){assert(pet_prefetch_take(g_h264_prefetch_job.ticket,g_h264_prefetch_job.cancel_epoch+1));assert(pet_prefetch_release(g_h264_prefetch_job.ticket));}
 assert(pet_prefetch_retire(&g_h264_prefetch_lifetime));
 if(g_h264_prefetch_job.source)g_h264_prefetch_job.source->decoder_readers--;
 pet_resources_device_revoke(g_h264_prefetch_job.owner);pet_resources_device_finish(g_h264_prefetch_job.lease);
 memset(&g_h264_prefetch_job,0,sizeof(g_h264_prefetch_job));
}
int main(void){
 pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_ASSET_BUFFER),&owner)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(owner,PET_RESOURCE_ASSET_BUFFER,384,&lease)==PET_RESOURCE_OK);
 pet_p4_asset_cache_entry_t source={.bytes=pixels,.finished=true,.buffer_lease=lease,.owner=owner};memset(pixels,0x5a,sizeof(pixels));
 /* EMPTY first frame: no packed output required; no claim/notify before tail. */
 h264_frame_offer_begin();assert(h264_prefetch_offer(&source,"first",16,16,true,0));
 assert(source.decoder_readers==1&&!notifications&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_EMPTY);
 bool execute=false;assert(!pet_prefetch_claim(g_h264_prefetch_job.ticket,&execute));
 assert(!h264_prefetch_offer(&source,"duplicate",16,16,true,0));
 h264_frame_offer_end(true);assert(!g_h264_frame_offer.active&&!g_h264_frame_offer.pending&&!g_h264_frame_offer.source);
 assert(notifications==1&&source.decoder_readers==1&&g_h264_prefetch_lifetime.phase==PET_PREFETCH_QUEUED);
 /* WORKING must not reoffer. READY future target is held untouched. */
 assert(pet_prefetch_claim(g_h264_prefetch_job.ticket,&execute)&&execute);
 h264_frame_offer_begin();assert(!h264_prefetch_offer(&source,"working",16,16,false,1));h264_frame_offer_end(true);assert(notifications==1);
 assert(pet_prefetch_complete(g_h264_prefetch_job.ticket,true));
 h264_frame_offer_begin();assert(!h264_prefetch_offer(&source,"future",16,16,false,1));h264_frame_offer_end(true);
 assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_READY&&notifications==1);retire();assert(!source.decoder_readers);
 /* Cancellation/storage changes before flush and in the final broker/offer
  * gap cannot launder an old intent into the freshly sampled epoch. */
 for(unsigned scenario=0;scenario<7;scenario++){
  h264_frame_offer_begin();assert(h264_prefetch_offer(&source,"cancel",16,16,false,1));
  if(scenario==0)g_asset_prepare_cancel_epoch++;
  if(scenario==1)g_asset_prepare_storage_epoch++;
  if(scenario==2)g_asset_prepare_suspended=true;
  if(scenario==3)hook=1;
  if(scenario==4)hook=2;
  if(scenario==5)suspended_audio=true;
  h264_frame_offer_end(scenario!=6); /* rotate/LCD failure drops the pin */
  assert(!source.decoder_readers&&!g_h264_frame_offer.pending&&!g_h264_frame_offer.source);
  assert(g_h264_prefetch_lifetime.phase==PET_PREFETCH_EMPTY&&notifications==1);
  g_asset_prepare_suspended=suspended_audio=false;
 }
 /* Explicit cleanup is deferred too; it has no source pin. */
 h264_frame_offer_begin();assert(h264_prefetch_offer(NULL,NULL,0,0,true,-1));assert(notifications==1);h264_frame_offer_end(true);assert(notifications==2);retire();
 /* Revoked source authority drops without publishing or freeing its pixels. */
 h264_frame_offer_begin();assert(h264_prefetch_offer(&source,"revoked",16,16,false,1));asset_cache_release(&source);assert(source.bytes==pixels&&!freed&&source.decoder_readers==1);h264_frame_offer_end(true);
 assert(!source.decoder_readers&&notifications==2);for(unsigned n=0;n<sizeof(pixels);n++)assert(pixels[n]==0x5a);
 pet_resources_device_finish(lease);pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.live_owners&&!usage.revoked_owners);
 return 0;
}
'''
    compile_run(tmp_path,code,'adapter')

def test_frame_tail_and_rotate_error_cleanup_are_inside_actual_renderer():
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    body=function(source,'pet_p4_renderer_render')
    assert body.index('h264_frame_offer_begin()')<body.index('render_asset_pet_frame(')
    assert body.index('record_render_performance(')<body.index('h264_frame_offer_end(lcd_err==ESP_OK)')<body.rindex('return lcd_err')
    assert 'h264_frame_offer_end(false)' in body
    assert 'ESP_RETURN_ON_ERROR(rotate_landscape_to_panel()' not in body
