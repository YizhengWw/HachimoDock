"""R145 main-frame parent: the main-page H.264 pack moves from the render frame onto the existing priority-1 prefetch
worker (two static PSRAM back buffers, adopt-at-take, unchanged render-thread pack as the counted fallback). Flag gating
with #error, env chain pins, structure, byte-exactness of the worker pack against an independent reference and the
original I420 pixel path, and the deterministic adapter interleavings with the flag ON. Offline only."""
from pathlib import Path
import configparser
import re
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]
FLAG='PET_P4_H264_WORKER_PACK'
CHAIN=['PET_P4_APP_DIRTY_CACHE','PET_P4_APP_CLEAR_OPAQUE_CENTER','PET_P4_APP_COLD_FAST','PET_P4_APP_PPA_COPY']
RTOS_FORBIDDEN=('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY','CONFIG_')

def _cc(args,src):
    return subprocess.run(['cc','-std=c11','-fsyntax-only','-I',str(ROOT/'main'),*args,str(src)],capture_output=True,text=True)

def test_flag_is_default_off_and_requires_ppa_copy_parent_and_prefetch_worker(tmp_path):
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_render_lifetime.h"\nint main(void){return %s;}\n'%FLAG)
    alone=_cc([f'-D{FLAG}=1'],src)
    assert alone.returncode!=0 and 'Worker-side H264 pack requires the PPA frame copy parent' in alone.stderr
    chain=[f'-D{d}=1' for d in CHAIN]
    no_prefetch=_cc([*chain,f'-D{FLAG}=1'],src)
    assert no_prefetch.returncode!=0 and 'requires the native prefetch worker' in no_prefetch.stderr
    assert _cc(['-Wall','-Wextra','-Werror',*chain,'-DPET_P4_H264_PREFETCH=1',f'-D{FLAG}=1'],src).returncode==0
    assert _cc(['-Wall','-Wextra','-Werror'],src).returncode==0
    binary=tmp_path/'off'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    assert subprocess.run([str(binary)]).returncode==0
    # Lifetime fields and JSON keys exist only behind the flag.
    lifetime=(ROOT/'main/pet_p4_render_lifetime.h').read_text();diag=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    block=lifetime[lifetime.index(f'#if {FLAG}'):];block=block[:block.index('#endif')]
    assert 'uint32_t worker_packs,worker_pack_fallbacks,worker_pack_max_us,worker_pack_last_us;' in block
    guarded=re.findall(r'^#if '+FLAG+r'\s*$(.*?)^#endif',diag,re.M|re.S)
    jblock=next(block for block in guarded if '"workerPack"' in block)
    offer_block=next(block for block in guarded if '"lastOfferUs"' in block)
    assert '"maxOfferUs"' in offer_block
    assert 'render.prefetch.last_offer_us' in offer_block and 'render.prefetch.max_offer_us' in offer_block
    for key in ('"workerPack"','"frames"','"fallbacks"','"maxUs"','"lastUs"'):assert key in jblock,key

def test_env_chain_main_frame_parent_rides_framework_parent_and_carries_the_public_candidate():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    env=config['env:esp32_p4_main_frame_parent']
    assert env['extends']=='env:esp32_p4_framework_parent' and env['custom_p4_project_version']=='0.7.103-p4-r145-mainframe'
    lines=[l.strip() for l in env['build_flags'].split('\n') if l.strip()]
    assert lines==['${env:esp32_p4_framework_parent.build_flags}',f'-D{FLAG}=1']
    assert not any(v in env['build_flags'] for v in RTOS_FORBIDDEN)
    assert set(env.keys())-set(config['DEFAULT'].keys())=={'extends','custom_p4_project_version','build_flags'}
    # r150: the shared capability parent rides this env and carries the public candidate.
    capability=config['env:esp32_p4_capability_parent']
    assert capability['extends']=='env:esp32_p4_main_frame_parent'
    assert [l.strip() for l in capability['build_flags'].split('\n') if l.strip()][0]=='${env:esp32_p4_main_frame_parent.build_flags}'
    candidate=config['env:esp32_p4_program_public_candidate']
    assert candidate['extends']=='env:esp32_p4_capability_parent'
    assert [l.strip() for l in candidate['build_flags'].split('\n') if l.strip()][0]=='${env:esp32_p4_capability_parent.build_flags}'
    versions=[config[s]['custom_p4_project_version'] for s in config.sections() if 'custom_p4_project_version' in config[s]]
    assert versions.count('0.7.103-p4-r145-mainframe')==1
    for name in config.sections():
        text=config[name].get('extends','')+config[name].get('build_flags','')
        if name=='env:esp32_p4_main_frame_parent':continue
        assert f'{FLAG}=1' not in text,name
        if name!='env:esp32_p4_capability_parent':assert 'esp32_p4_main_frame_parent' not in text,name
    assert (ROOT/'sdkconfig.esp32_p4_main_frame_parent').read_bytes()==(ROOT/'sdkconfig.esp32_p4_framework_parent').read_bytes()

def test_worker_pack_structure_keeps_phases_buffers_and_fallback_in_place():
    inc=(ROOT/'main/pet_p4_h264_prefetch.inc').read_text();renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    # Two static PSRAM buffers at the logical frame size, cache-line isolated; no heap, no internal RAM.
    assert 'static EXT_RAM_BSS_ATTR __attribute__((aligned(128))) uint8_t g_h264_worker_packed[2][PET_P4_H264_WORKER_PACKED_BYTES];' in inc
    assert '#define PET_P4_H264_WORKER_PACKED_BYTES ((size_t)PET_P4_UI_WIDTH*PET_P4_UI_HEIGHT*3/2)' in inc
    pack=function(inc,'h264_worker_pack')
    for forbidden in ('vTaskPrioritySet','heap_caps','malloc(','vTaskDelay','portENTER_CRITICAL','g_h264_packed_current','g_framebuffer'):
        assert forbidden not in pack,forbidden
    # Same half/half split with the existing pack task and the same row kernel, joined before publication.
    assert pack.index('g_h264_pack_job=primary;')<pack.index('xTaskNotifyGive(g_h264_pack_task);')<pack.index('pack_h264_i420_rows(&primary);')<pack.index('xSemaphoreTake(g_h264_pack_done,portMAX_DELAY);')<pack.index('job->packed_ok=true;')
    assert 'required>job->packed_capacity' in pack and 'resolve_h264_i420_layout(job->width,job->height,job->output_size,&stride,&storage_h)' in pack
    # Worker: pack only after an authoritative decode, before any decoder reset or lease finish.
    worker=function(inc,'h264_prefetch_worker')
    assert worker.index('ok=g_h264_decoded_frame==job.frame;')<worker.index('if(ok){h264_worker_pack(&job);ok=job.source&&asset_prepare_current(&source);}')<worker.index('if(!ok)reset_h264_decoder();')<worker.index('pet_resources_device_finish(job.lease)')
    # Offer: real decode jobs get the back buffer (never the displayed front); cleanup offers get none.
    offer=function(inc,'h264_prefetch_offer')
    assert 'if(source){next.packed=h264_worker_back_buffer();next.packed_capacity=PET_P4_H264_WORKER_PACKED_BYTES;}' in offer
    assert offer.index('next.packed=h264_worker_back_buffer()')<offer.index('portENTER_CRITICAL(&g_asset_prepare_mux)')
    back=function(inc,'h264_worker_back_buffer')
    assert 'g_h264_packed_current.data==g_h264_worker_packed[0]?g_h264_worker_packed[1]:g_h264_worker_packed[0]' in back
    # Take: adopt first, original render-thread pack kept as the counted fallback, same PPA convert afterwards.
    take=function(inc,'render_h264_asset_frame_async')
    adopt=take.index('h264_worker_pack_adopt(&ready,asset->width,asset->height,stride,storage_h)')
    assert take.index('pet_prefetch_take(ready.ticket')<adopt<take.index('pet_render_counter_increment(&g_worker_pack_fallbacks);')<take.index('pack_h264_i420_for_ppa(ready.output,')<take.index('pet_prefetch_release(ready.ticket)')
    assert take.count('scale_h264_frame_ppa(g_h264_packed_current.data,')==1 and take.count('pack_h264_i420_for_ppa(ready.output,')==2
    adopt_fn=function(inc,'h264_worker_pack_adopt')
    assert 'ready->packed_stride!=stride||ready->packed_storage_h!=storage_h' in adopt_fn and 'pet_h264_packed_view_init(&g_h264_packed_current,ready->packed,ready->packed_capacity' in adopt_fn
    assert 'memcpy' not in adopt_fn
    # Job fields, counters and lifetime publication live behind the flag in the renderer.
    job=renderer[renderer.index('typedef struct {\n  pet_prefetch_ticket_t ticket;'):renderer.index('} pet_h264_prefetch_job_t;')]
    assert f'#if {FLAG}' in job and 'uint8_t *packed;size_t packed_capacity;' in job and 'int packed_stride,packed_storage_h;bool packed_ok;uint32_t pack_us;' in job
    assert 'static uint32_t g_worker_packs,g_worker_pack_fallbacks,g_worker_pack_max_us,g_worker_pack_last_us;' in renderer
    record=function(renderer,'record_render_performance')
    assert 'g_metrics.lifetime.worker_packs=g_worker_packs;' in record and 'g_metrics.lifetime.worker_pack_max_us=g_worker_pack_max_us;' in record
    # The original render-thread pack job and its boost are untouched.
    original=function(renderer,'pack_h264_i420_for_ppa')
    assert original.index('xTaskNotifyGive(')<original.index('xSemaphoreTake(g_h264_pack_done, portMAX_DELAY)')<original.index('*packed =')
    assert 'pet_p4_render_can_boost' in original
    # No task, priority, clock or watchdog policy changes anywhere in the touched sources.
    for path in ('main/pet_p4_h264_prefetch.inc','main/pet_p4_render_policy.h','main/pet_p4_render_lifetime.h'):
        text=(ROOT/path).read_text()
        for forbidden in ('xTaskCreate','vTaskPrioritySet','esp_task_wdt','rtc_clk','esp_pm_'):assert forbidden not in text,(path,forbidden)

def test_worker_pack_is_byte_exact_against_reference_and_original_i420_pixels_in_split_and_single_modes(tmp_path):
    renderer=(ROOT/'main/pet_p4_renderer.c').read_text();inc=(ROOT/'main/pet_p4_h264_prefetch.inc').read_text()
    code=r'''
#include "pet_p4_h264_packed.h"
#include "pet_p4_h264_pack_word.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdFALSE 0
typedef int TaskHandle_t;typedef void *SemaphoreHandle_t;
typedef struct {const uint8_t *plane_y,*plane_u,*plane_v;uint8_t *packed;size_t packed_stride;int src_w,src_stride,row_begin,row_end;}pet_p4_h264_pack_job_t;
typedef struct {int width,height;uint32_t output_size;uint8_t *output;uint8_t *packed;size_t packed_capacity;int packed_stride,packed_storage_h;bool packed_ok;uint32_t pack_us;}pet_h264_prefetch_job_t;
static TaskHandle_t g_h264_pack_task;static SemaphoreHandle_t g_h264_pack_done;static pet_p4_h264_pack_job_t g_h264_pack_job;
static bool pack_given;static unsigned split_packs,rows_packed;static int64_t clock_us;
static uint16_t framebuffer[640*480],reference[640*480];static uint16_t *g_framebuffer=framebuffer;
static int destination_width,destination_height;
static bool asset_fit_layout(int sw,int sh,int *x,int *y,int *w,int *h,float *scale){
 (void)sw;(void)sh;(void)scale;*w=destination_width;*h=destination_height;*x=(640-*w)/2;*y=(480-*h)/2;return true;
}
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r&248U)<<8)|((g&252U)<<3)|(b>>3));}
static int64_t esp_timer_get_time(void){return clock_us+=7;}
static uint32_t elapsed_us_clamped(int64_t from,int64_t to){return to>=from?(uint32_t)(to-from):0;}
static void actual_pack_rows(const pet_p4_h264_pack_job_t *job);
static void pack_h264_i420_rows(const pet_p4_h264_pack_job_t *job){rows_packed+=(unsigned)(job->row_end-job->row_begin);actual_pack_rows(job);}
static void xTaskNotifyGive(TaskHandle_t task){assert(task==g_h264_pack_task&&!pack_given);split_packs++;pack_h264_i420_rows(&g_h264_pack_job);pack_given=true;}
static int xSemaphoreTake(SemaphoreHandle_t sem,unsigned timeout){assert(sem==g_h264_pack_done);if(pack_given){pack_given=false;return pdTRUE;}assert(timeout==0);return pdFALSE;}
'''+function(renderer,'clamp_u8')+function(renderer,'resolve_h264_i420_layout')+function(renderer,'pack_h264_i420_rows').replace('pack_h264_i420_rows(','actual_pack_rows(',1)+function(renderer,'scale_h264_frame_cpu')+function(inc,'scale_h264_packed_cpu')+function(inc,'h264_worker_pack')+r'''
static void independent_reference(const uint8_t *i420,int w,int stride,int storage_h,uint8_t *dest){
 const uint8_t *py=i420,*pu=i420+(size_t)stride*storage_h,*pv=pu+(size_t)stride*storage_h/4;
 for(int row=0;row<storage_h;row++){
  const uint8_t *y=py+(size_t)row*stride,*uv=((row&1)?pv:pu)+(size_t)(row/2)*(stride/2);
  uint8_t *at=dest+(size_t)row*((size_t)w*3/2);
  for(int x=0;x<w;x+=2){*at++=uv[x/2];*at++=y[x];*at++=y[x+1];}
 }
}
int main(void){
 unsigned random=1234567;
 const int widths[]={16,32,96,320,480,640},heights[]={16,32,64,240,352,480};
 for(unsigned mode=0;mode<2;mode++)for(unsigned i=0;i<6;i++)for(unsigned j=0;j<6;j++)for(int pad=0;pad<2;pad++){
  int w=widths[i],h=heights[j],stride=(w+15)&~15,storage_h=h+pad*16;
  size_t source_size=(size_t)stride*storage_h*3/2,required=(size_t)w*3/2*(size_t)storage_h,capacity=(size_t)640*480*3/2;
  uint8_t *source=malloc(source_size),*packed=calloc(capacity+64,1),*expected=calloc(capacity+64,1);assert(source&&packed&&expected);
  memset(packed,0x5a,capacity+64);
  for(size_t n=0;n<source_size;n++){random=random*1664525U+1013904223U;source[n]=(uint8_t)(random>>24);}
  g_h264_pack_task=mode?3:0;g_h264_pack_done=mode?(void*)&g_h264_pack_done:NULL;
  pet_h264_prefetch_job_t job={w,h,(uint32_t)source_size,source,packed,capacity,0,0,false,0};
  unsigned splits=split_packs;rows_packed=0;
  h264_worker_pack(&job);
  if(required>capacity){ /* a padded full-size frame does not fit: untouched, render thread falls back */
   assert(!job.packed_ok&&!job.pack_us&&!rows_packed);for(size_t n=0;n<capacity+64;n++)assert(packed[n]==0x5a);
   free(source);free(packed);free(expected);continue;
  }
  assert(job.packed_ok&&job.packed_stride==stride&&job.packed_storage_h==storage_h&&job.pack_us>0);
  assert(split_packs==splits+(mode?1:0)&&rows_packed==(unsigned)storage_h&&!pack_given);
  memset(expected,0x5a,capacity+64);independent_reference(source,w,stride,storage_h,expected);
  assert(!memcmp(packed,expected,capacity+64)); /* exact bytes; nothing beyond `required` touched */
  pet_h264_packed_view_t view={0};assert(pet_h264_packed_view_init(&view,packed,capacity,(size_t)w*3/2,w,h,storage_h));
  destination_width=(i%2)?640:317;destination_height=(j%2)?480:239;
  scale_h264_frame_cpu(source,w,h,stride,storage_h);memcpy(reference,framebuffer,sizeof(reference));
  memset(framebuffer,0xff,sizeof(framebuffer));scale_h264_packed_cpu(&view);
  assert(!memcmp(reference,framebuffer,sizeof(reference))); /* same pixels as the original I420 path */
  /* Too small a back buffer: no write at all, job not packed, render thread falls back. */
  memset(packed,0x5a,capacity+64);job=(pet_h264_prefetch_job_t){w,h,(uint32_t)source_size,source,packed,required-1,0,0,true,99};
  h264_worker_pack(&job);assert(!job.packed_ok&&!job.pack_us);
  for(size_t n=0;n<capacity+64;n++)assert(packed[n]==0x5a);
  /* Invalid layout (odd output size) is rejected before any write. */
  job=(pet_h264_prefetch_job_t){w,h,(uint32_t)source_size+1,source,packed,capacity,0,0,true,99};
  h264_worker_pack(&job);assert(!job.packed_ok);
  job=(pet_h264_prefetch_job_t){w,h,(uint32_t)source_size,source,NULL,capacity,0,0,true,99};
  h264_worker_pack(&job);assert(!job.packed_ok);
  for(size_t n=0;n<capacity+64;n++)assert(packed[n]==0x5a);
  free(source);free(packed);free(expected);
 }
 return 0;
}
'''
    src=tmp_path/'exact.c';src.write_text(code);binary=tmp_path/'exact'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=120)

def test_actual_adapter_with_worker_pack_alternates_buffers_never_packs_on_the_render_thread_and_keeps_every_cancellation_rule(tmp_path):
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    declarations=source[source.index('static esp_h264_dec_handle_t g_h264_decoder;'):source.index('static uint8_t *g_h264_ppa_input;')]
    declarations+='\nstatic uint8_t *g_h264_ppa_input;static size_t g_h264_ppa_input_capacity;static char g_h264_fs_path[80];\n'
    declarations+=source[source.index('typedef struct {\n  uint8_t *bytes;'):source.index('static bool h264_stream_has_single_slice_access_units(')]
    functions='\n'.join(function(source,name) for name in (
        'asset_prepare_audio_busy','asset_prepare_current','asset_cache_release','pet_p4_renderer_cancel_asset_preparation',
        'pet_p4_renderer_set_asset_preparation_suspended','reset_h264_decoder','reset_h264_decoder_traced',
        'start_h264_decoder_role','decode_next_h264_frame_role'))
    functions+='\n'+function(source,'pack_h264_i420_rows').replace('pack_h264_i420_rows(','actual_pack_rows(',1)
    harness=(ROOT/'tests/p4_main_frame_adapter_test.c').read_text()
    assert harness.count('/* ACTUAL_DECLARATIONS */')==1 and harness.count('/* ACTUAL_FUNCTIONS */')==1
    code=harness.replace('/* ACTUAL_DECLARATIONS */',declarations).replace('/* ACTUAL_FUNCTIONS */',functions)
    src=tmp_path/'adapter.c';src.write_text(code);binary=tmp_path/'adapter'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
      *[f'-D{d}=1' for d in CHAIN],'-DPET_P4_H264_PREFETCH=1',f'-D{FLAG}=1',
      '-I',str(ROOT/'main'),str(src),str(ROOT/'main/pet_p4_resources.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=35)
