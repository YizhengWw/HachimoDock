"""Offline staging regression; no SPIFFS writes or device access."""
import ctypes as C
import subprocess
from pathlib import Path


def test_staging_rejects_interleaving_and_changed_retransmission(tmp_path):
    root = Path(__file__).resolve().parents[1]
    (tmp_path / 'freertos').mkdir()
    (tmp_path / 'esp_heap_caps.h').write_text('#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_malloc(n,c) malloc(n)\n')
    (tmp_path / 'esp_log.h').write_text('#define ESP_LOGW(tag,...) ((void)(tag))\n')
    (tmp_path / 'freertos/FreeRTOS.h').write_text('#include <assert.h>\n#define configASSERT(x) assert(x)\n#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffu\n')
    (tmp_path / 'freertos/semphr.h').write_text('typedef void *SemaphoreHandle_t;\nstatic inline void *xSemaphoreCreateMutex(void){return (void*)1;}\n#define xSemaphoreTake(a,b) ((void)(a), (void)(b), 1)\n#define xSemaphoreGive(a) ((void)(a))\n#define vSemaphoreDelete(a) ((void)(a))\n')
    output = tmp_path / 'store.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable', '-I', str(tmp_path), '-I', str(root/'main'), '-I', str(root/'tests/vendor/cjson'), *[str(root/'main'/f) for f in ('pet_p4_component_media.c', 'pet_p4_component_ext.c', 'pet_p4_component_events.c', 'pet_p4_sfx_core.c')], str(root/'tests/vendor/cjson/cJSON.c'), '-o', str(output)], check=True)
    lib = C.CDLL(str(output))
    lib.pet_cmedia_stage_chunk.argtypes = [C.c_int, C.c_char_p, C.c_uint32, C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t]
    lib.pet_cmedia_stage_chunk.restype = C.c_bool
    def chunk(kind, name, index, data):
        return lib.pet_cmedia_stage_chunk(kind, name, index, data, len(data), None, 0)
    lib.pet_cmedia_stage_reset()
    assert not chunk(99, b'a', 0, b'x')
    assert not chunk(1, b'a'*16, 0, b'x')
    assert chunk(1, b'a', 0, b'P4A1')
    assert chunk(1, b'a', 0, b'P4A1')
    assert not chunk(1, b'a', 0, b'FAIL')
    assert not chunk(1, b'b', 0, b'P4A1')
    assert chunk(1, b'a', 1, b'\1\0\0\0')
    assert chunk(1, b'a', 2, b'\0\0')
    lib.pet_cmedia_stage_reset()
    assert chunk(1, b'a', 0, b'P4A1\1\0\0\0\0\0')
    assert not chunk(1, b'a', 1, b'x')
    lib.pet_cmedia_stage_reset()


def test_store_generation_leases_with_real_threads_and_address_sanitizer(tmp_path):
    root = Path(__file__).resolve().parents[1]
    (tmp_path/'freertos').mkdir()
    (tmp_path/'esp_heap_caps.h').write_text('#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_malloc(n,c) malloc(n)\n')
    (tmp_path/'esp_log.h').write_text('#define ESP_LOGW(tag,...) ((void)(tag))\n')
    (tmp_path/'freertos/FreeRTOS.h').write_text('#include <assert.h>\n#define configASSERT(x) assert(x)\n#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffu\n')
    (tmp_path/'freertos/semphr.h').write_text('''#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){SemaphoreHandle_t p=malloc(sizeof(*p));if(p)pthread_mutex_init(p,NULL);return p;}
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned t){return (t ? pthread_mutex_lock(p) : pthread_mutex_trylock(p))==0;}
static inline int xSemaphoreGive(SemaphoreHandle_t p){return pthread_mutex_unlock(p)==0;}
static inline void vSemaphoreDelete(SemaphoreHandle_t p){pthread_mutex_destroy(p);free(p);}
''')
    harness = tmp_path/'leases.c'
    harness.write_text(r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <sched.h>
static atomic_bool pause_io;
static pthread_mutex_t io_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_cond=PTHREAD_COND_INITIALIZER;
static int io_phase;
static void pause_prepare_io(void);
static FILE *mapped_fopen(const char *p,const char *mode){if(atomic_load(&pause_io))pause_prepare_io();if(!strncmp(p,"/spiffs/",8))p+=8;return fopen(p,mode);}
static int mapped_stat(const char *p,struct stat *s){if(!strncmp(p,"/spiffs/",8))p+=8;return stat(p,s);}
#define fopen mapped_fopen
#define stat mapped_stat
#include "pet_p4_component_media.c"
#undef fopen
#undef stat
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int state;
static pet_cext_runtime_t runtime;
static void *reader(void *unused){
 (void)unused; pet_cmedia_sample_lease_t sample={0};pet_cmedia_clip_lease_t clip={0};
 assert(pet_cmedia_sample_pin(0,&sample));assert(pet_cmedia_clip_pin(0,&clip));
 uint32_t old=sample.generation;
 pthread_mutex_lock(&gate);state=1;pthread_cond_broadcast(&cond);
 while(state<2)pthread_cond_wait(&cond,&gate);
 pthread_mutex_unlock(&gate);
 for(unsigned i=0;i<10000;i++){assert(sample.pcm[0]==1234);assert(clip.stream[0]==0x65);}
 pet_cmedia_sample_lease_t stale={0};assert(!pet_cmedia_sample_pin_at(0,old,&stale));
 assert(pet_cmedia_unpin(sample.token));assert(!pet_cmedia_unpin(sample.token));
 assert(pet_cmedia_unpin(clip.token));return NULL;
}
static void fixture(void){
 FILE *f=fopen("p4m00-0-s0.bin","wb");assert(f);unsigned char sample[]={ 'P','4','A','1',1,0,0,0,0xd2,4};assert(fwrite(sample,1,sizeof(sample),f)==sizeof(sample));fclose(f);
 f=fopen("p4m00-0-c0.bin","wb");assert(f);unsigned char clip[]={'P','4','V','1',160,0,120,0,15,0,1,0,1,0,0,0,0x65};assert(fwrite(clip,1,sizeof(clip),f)==sizeof(clip));fclose(f);
 runtime.sample_count=1;runtime.clip_count=1;strcpy(runtime.clips[0].id,"clip");
}
int main(void){
 fixture();assert(pet_cmedia_load(0,0,&runtime));
 pthread_t thread;assert(!pthread_create(&thread,NULL,reader,NULL));
 pthread_mutex_lock(&gate);while(state<1)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
 pet_cmedia_unload();assert(!pet_cmedia_sample_pcm(0,NULL));
 assert(pet_cmedia_load(0,0,&runtime));pet_cmedia_sample_lease_t b={0};assert(pet_cmedia_sample_pin(0,&b));
 assert(pet_cmedia_load(0,0,&runtime));pet_cmedia_sample_lease_t c={0};assert(pet_cmedia_sample_pin(0,&c));
 uint32_t current=pet_cmedia_generation();assert(!pet_cmedia_load(0,0,&runtime));assert(pet_cmedia_generation()==current);
 assert(!pet_cmedia_load(1,0,&runtime));assert(pet_cmedia_generation()==current);
 pthread_mutex_lock(&gate);state=2;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);pthread_join(thread,NULL);
 assert(pet_cmedia_load(0,0,&runtime));
 assert(pet_cmedia_unpin(b.token));assert(pet_cmedia_unpin(c.token));
 pet_cmedia_sample_lease_t pins[PET_CMEDIA_LEASES];
 for(unsigned i=0;i<PET_CMEDIA_LEASES;i++)assert(pet_cmedia_sample_pin(0,&pins[i]));
 pet_cmedia_sample_lease_t extra={0};assert(!pet_cmedia_sample_pin(0,&extra));
 pet_cmedia_unload();for(unsigned i=0;i<PET_CMEDIA_LEASES;i++){assert(pins[i].pcm[0]==1234);assert(pet_cmedia_unpin(pins[i].token));}
 for(unsigned i=0;i<PET_CMEDIA_BANKS;i++)assert(!g_banks[i].samples&&!g_banks[i].clip&&!g_banks[i].pins);
 pet_cmedia_stats_t stats={0};assert(pet_cmedia_stats(&stats));
 assert(!stats.live_tokens&&!stats.bank_pins&&!stats.live_banks);
 assert(stats.tokens_issued==stats.tokens_reclaimed&&stats.deferred_reclaimed>0);
 // Empty media stages add no bytes, even if existing media exceeds budget.
 FILE *f=fopen("p4m31-1-b0.bin","wb");assert(f);assert(!ftruncate(fileno(f),PET_CMEDIA_DEVICE_BUDGET_BYTES));fclose(f);
 assert(pet_cmedia_persist(0,0,NULL));
 // A real incoming sample must still fail the device-wide peak budget.
 unsigned char incoming[]={'P','4','A','1',1,0,0,0,0,0};
 pet_cext_runtime_t adding={.sample_count=1};strcpy(adding.sample[0].id,"new");
 assert(pet_cmedia_stage_chunk(PET_CEXT_MEDIA_SAMPLE,"new",0,incoming,sizeof(incoming),NULL,0));
 assert(!pet_cmedia_persist(1,0,&adding));pet_cmedia_stage_reset();
 g_generation=UINT32_MAX;pet_cmedia_unload();assert(!pet_cmedia_generation());assert(!pet_cmedia_load(0,0,&runtime));
 return 0;
}
''')
    audio_source = (root/'main/pet_p4_audio.c').read_text()
    start = audio_source.index('#if PET_P4_SEQUENCE || PET_P4_SAMPLES', audio_source.index('bool pet_p4_audio_sfx_trigger'))
    audio_adapter = audio_source[start:audio_source.index('#else\n#define CEXT_ENABLED()', start)] + '\n#endif\n'
    miniapp_source = (root/'main/pet_p4_miniapp_ext.inc').read_text()
    start = miniapp_source.index('static void miniapp_ext_audio_apply(')
    miniapp_adapter = miniapp_source[start:miniapp_source.index('#if PET_P4_EVENTS', start)]
    start = miniapp_source.index('static pet_cext_publish_ticket_t *g_ext_pending_publish')
    publish_adapter = miniapp_source[start:miniapp_source.index('static void miniapp_ext_installed', start)]
    renderer_source = (root/'main/pet_p4_renderer_ext.inc').read_text()
    start = renderer_source.index('#if PET_P4_CLIP\n')
    clip_adapter = 'static uint32_t g_ext_snapshot_generation;\nvoid renderer_ext_media_snapshot(uint32_t g){g_ext_snapshot_generation=g;}\n' + renderer_source[start:]
    stubs = r'''
#define PET_P4_SEQUENCE 1
#define PET_P4_SAMPLES 1
#define PET_P4_CLIP 1
#define EXT_RAM_BSS_ATTR
static atomic_bool g_capture_requested,g_conversation;
static atomic_uint g_playback_generation;
static SemaphoreHandle_t g_stream_lock;
static void *g_playback_queue;
static bool pet_p4_audio_playback_ready(void){return true;}
#define xSemaphoreTakeRecursive xSemaphoreTake
#define xSemaphoreGiveRecursive xSemaphoreGive
#define xQueueReset(p) ((void)(p))
#include "pet_p4_prefetch_lifetime.h"
typedef struct {unsigned generation;} pet_resource_owner_t;
typedef unsigned pet_resource_lease_t;
typedef struct {uint8_t *bytes;size_t size;bool finished,h264,single_slice_valid;unsigned decoder_readers;char fs_path[80];pet_resource_owner_t owner;pet_resource_lease_t buffer_lease;} pet_p4_asset_cache_entry_t;
#define PET_RESOURCE_ASSET_BUFFER 1
#define PET_RESOURCE_OK 0
#define PET_RESOURCE_MASK(x) (x)
static int pet_resources_device_owner_open(unsigned m,pet_resource_owner_t *o){(void)m;o->generation=1;return 0;}
static int pet_resources_device_acquire(pet_resource_owner_t o,unsigned k,uint32_t n,pet_resource_lease_t *l){(void)o;(void)k;(void)n;*l=1;return 0;}
static void pet_resources_device_revoke(pet_resource_owner_t o){(void)o;}
static int pet_resources_device_finish(pet_resource_lease_t l){(void)l;return 0;}
static pthread_mutex_t g_asset_prepare_mux=PTHREAD_MUTEX_INITIALIZER;
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
static pet_prefetch_lifetime_t g_h264_prefetch_lifetime;
static bool decoder_finished;
typedef struct {pet_cext_runtime_t cext;bool active;char widget_id[64];int var_count;struct{int32_t int_value;}vars[4];struct{char title[64];}view;} miniapp_runtime_t;
typedef struct {pet_cmedia_ticket_t media;miniapp_runtime_t *next;} pet_cext_publish_ticket_t;
static bool g_ext_media_transition;
static _Thread_local unsigned ext_depth;
static pthread_mutex_t ext_mutex=PTHREAD_MUTEX_INITIALIZER;
static void ext_lock(void){pthread_mutex_lock(&ext_mutex);ext_depth++;}
static void ext_unlock(void){assert(ext_depth==1);ext_depth--;pthread_mutex_unlock(&ext_mutex);}
static bool h264_prefetch_reset(void){return decoder_finished;}
typedef struct {int x,y,w,h;uint16_t color;} pet_surface_node_t;
typedef struct {const uint8_t *data;} pet_h264_packed_view_t;
typedef struct {char path[80];pet_prefetch_ticket_t ticket;uint64_t cancel_epoch;unsigned output_size;const uint8_t *output;int frame;} pet_h264_prefetch_job_t;
static pet_h264_prefetch_job_t g_h264_prefetch_job;
static pet_h264_packed_view_t g_h264_packed_current;
static unsigned g_h264_ppa_input_capacity;
static char g_h264_presented_path[80];
static uint16_t framebuffer[640*480],*g_framebuffer=framebuffer;
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static bool pet_resources_device_current(pet_resource_lease_t l){return l!=0;}
static unsigned frame_fills;
static void fill_rect(int x,int y,int w,int h,uint16_t c){(void)x;(void)y;(void)w;(void)h;(void)c;frame_fills++;}
static bool resolve_h264_i420_layout(int w,int h,unsigned b,int *s,int *t){(void)w;(void)h;(void)b;(void)s;(void)t;return false;}
static bool h264_worker_pack_adopt(const pet_h264_prefetch_job_t *j,int w,int h,int s,int t){(void)j;(void)w;(void)h;(void)s;(void)t;return false;}
static bool pack_h264_i420_for_ppa(const uint8_t *p,int w,int h,int s,int t,const uint8_t **out){(void)p;(void)w;(void)h;(void)s;(void)t;(void)out;return false;}
static bool pet_h264_packed_view_init(pet_h264_packed_view_t *v,const uint8_t *p,unsigned b,unsigned s,int w,int h,int t){(void)v;(void)p;(void)b;(void)s;(void)w;(void)h;(void)t;return false;}
static bool app_clip_contains(int x,int y){(void)x;(void)y;return true;}
static uint16_t pet_h264_packed_pixel(const pet_h264_packed_view_t *v,int x,int y){(void)v;(void)x;(void)y;return 0;}
static bool h264_prefetch_offer(pet_p4_asset_cache_entry_t *e,const char *p,int w,int h,bool r,int n){(void)e;(void)p;(void)w;(void)h;(void)r;(void)n;return false;}
'''
    # Exercise the real event cycle with an old active subscriber during IO.
    process_adapter = r'''
#undef PET_P4_EVENTS
#define PET_P4_EVENTS 1
#define PET_P4_MINIAPP_ACTION_MAX 64
static miniapp_runtime_t g_runtime;
static pthread_mutex_t model_mutex=PTHREAD_MUTEX_INITIALIZER;
static void miniapp_runtime_lock(void){assert(!ext_depth);pthread_mutex_lock(&model_mutex);}
static void miniapp_runtime_unlock(void){pthread_mutex_unlock(&model_mutex);}
static void copy_utf8(char *d,size_t n,const char *s){snprintf(d,n,"%s",s);}
static pet_cev_clock_snapshot_t miniapp_utc_snapshot(uint64_t now){(void)now;return (pet_cev_clock_snapshot_t){0};}
static pet_cev_bus_t g_ext_bus;
typedef struct{char widget_id[PET_CEV_WIDGET_ID_MAX],title[64];pet_cev_alarm_set_t alarms;bool used,dirty;uint64_t revision;} ext_alarm_entry_t;
static ext_alarm_entry_t g_ext_alarms[PET_CEV_COMPONENTS];
static bool g_ext_background_notify,g_ext_notice_pending;
static char g_ext_notice_title[64],g_ext_notice_text[PET_CEV_ALARM_TEXT_MAX],g_ext_notice_id[PET_CEV_WIDGET_ID_MAX],g_ext_notice_alarm[PET_CEV_NAME_MAX];
static unsigned event_dispatches;
static bool pet_p4_miniapp_dispatch_action(const char *a,uint64_t now){(void)now;assert(!strcmp(a,"event:done"));
 assert(!pthread_mutex_trylock(&ext_mutex));pthread_mutex_unlock(&ext_mutex);
 assert(!pthread_mutex_trylock(&model_mutex));pthread_mutex_unlock(&model_mutex);event_dispatches++;return true;}
'''
    # Use actual alarm bookkeeping and process, not a stand-in event probe.
    from p4_render_trace_test import function
    process_adapter += function(miniapp_source, 'ext_alarms_observe_locked') + '\n'
    process_adapter += function(miniapp_source, 'miniapp_ext_process') + '\n#undef PET_P4_EVENTS\n#define PET_P4_EVENTS 0\n'
    adapter_tests = r'''
static void pause_prepare_io(void){
 /* The real producer reaches fopen while media mutex is held, but neither
  * model nor ext may span that IO. */
 assert(!pthread_mutex_trylock(&ext_mutex));pthread_mutex_unlock(&ext_mutex);
 assert(!pthread_mutex_trylock(&model_mutex));pthread_mutex_unlock(&model_mutex);
 pthread_mutex_lock(&io_gate);io_phase=1;pthread_cond_broadcast(&io_cond);
 while(io_phase==1)pthread_cond_wait(&io_cond,&io_gate);
 pthread_mutex_unlock(&io_gate);
}
static miniapp_runtime_t preparing_next;
static pet_cext_publish_ticket_t preparing_ticket;
static bool prepared_result;
static void *prepare_producer(void *arg){(void)arg;prepared_result=miniapp_ext_prepare(&preparing_next,0,0,&preparing_ticket);return NULL;}
static void test_prepare_io_main_cycle(void){
 assert(pet_cmedia_load(0,0,&runtime));g_stream_lock=xSemaphoreCreateMutex();
 pet_p4_audio_cext_set(true,NULL,false,0,100);uint32_t gen=pet_cmedia_generation();
 g_runtime=(miniapp_runtime_t){.active=true,.cext=runtime};strcpy(g_runtime.widget_id,"old");
 g_runtime.cext.events=true;g_runtime.cext.emit_count=1;g_runtime.cext.emit_pending=1;strcpy(g_runtime.cext.emit_names[0],"done");
 char names[PET_CEV_NAMES_PER_PACKAGE][PET_CEV_NAME_MAX]={{"done"}};
 assert(pet_cev_bus_subscribe(&g_ext_bus,"old",names,1));
 preparing_next=(miniapp_runtime_t){.cext=runtime};preparing_ticket=(pet_cext_publish_ticket_t){0};
 atomic_store(&pause_io,true);io_phase=0;pthread_t producer;assert(!pthread_create(&producer,NULL,prepare_producer,NULL));
 pthread_mutex_lock(&io_gate);while(io_phase!=1)pthread_cond_wait(&io_cond,&io_gate);pthread_mutex_unlock(&io_gate);
 /* Producer cannot leave IO until this main-thread event/audio cycle returns. */
 miniapp_ext_process(100,true);assert(event_dispatches==1);
 pet_cext_audio_t snapshot={.enabled=false,.generation=gen};miniapp_ext_audio_apply_snapshot(&snapshot,NULL,gen);
 assert(atomic_load(&g_cext_enabled)); /* busy snapshot did not stop old audio */
 assert(g_ext_pending_publish==&preparing_ticket&&g_ext_publish_phase==EXT_PUBLISH_PREPARING);
 assert(!miniapp_ext_abort(&preparing_ticket));assert(!miniapp_ext_commit(&preparing_ticket));
 pet_cext_publish_ticket_t other={0};assert(!miniapp_ext_prepare(&preparing_next,0,0,&other));
 pthread_mutex_lock(&io_gate);assert(io_phase==1);atomic_store(&pause_io,false);io_phase=2;pthread_cond_broadcast(&io_cond);pthread_mutex_unlock(&io_gate);
 pthread_join(producer,NULL);assert(prepared_result&&g_ext_publish_phase==EXT_PUBLISH_READY);
 assert(miniapp_ext_abort(&preparing_ticket));assert(!preparing_ticket.media.serial&&!g_ext_pending_publish);
 assert(pet_cmedia_generation()==gen&&atomic_load(&g_cext_enabled));
 pet_p4_audio_cext_set(false,NULL,false,0,0);pet_cmedia_unload();vSemaphoreDelete(g_stream_lock);g_stream_lock=NULL;
}
static void *audio_stop_thread(void *arg){(void)arg;pet_p4_audio_cext_set(false,NULL,false,0,0);return NULL;}
static void test_audio_adapter(void){
 assert(pet_cmedia_load(0,0,&runtime));g_stream_lock=xSemaphoreCreateMutex();
 pet_cmedia_sample_lease_t sample={0};assert(pet_cmedia_sample_pin(0,&sample));
 pet_p4_audio_cext_set(true,NULL,false,0,100);
 assert(pet_p4_audio_cext_sample_lease(&sample));assert(!sample.token);
 assert(g_cext_sample_queue[0].token);int16_t pcm[320]={0};
 xSemaphoreTake(g_stream_lock,0);assert(cext_mix_step(pcm,false));xSemaphoreGive(g_stream_lock);
 assert(pcm[0]==1234);assert(!g_cext_voice_tokens[0]);assert(!g_cext_sample_queue[0].token);
 assert(pet_cmedia_sample_pin(0,&sample));
 assert(pet_cmedia_unpin(sample.token));
 for(unsigned i=0;i<3;i++){assert(pet_cmedia_sample_pin(0,&sample));assert(pet_p4_audio_cext_sample_lease(&sample));}
 assert(g_cext_sample_queued==2);
 pet_cmedia_unload(); // queue pins must protect buffers until reset
 pet_p4_audio_cext_set(false,NULL,false,0,0);pet_cmedia_collect();
 for(unsigned i=0;i<PET_CMEDIA_BANKS;i++)assert(!g_banks[i].pins&&!g_banks[i].samples);
 assert(!pet_p4_audio_cext_sample(sample.pcm,sample.frames));
 pet_seq_program_t sequence={.bpm=240,.track_count=1,.loop=false};
 sequence.tracks[0].count=1;sequence.tracks[0].volume=100;sequence.tracks[0].midi[0]=60;sequence.tracks[0].beats[0]=1;
 pet_p4_audio_cext_set(true,&sequence,true,100,100);
 for(unsigned i=0;i<14;i++){memset(pcm,0,sizeof(pcm));xSemaphoreTake(g_stream_lock,0);(void)cext_mix_step(pcm,false);xSemaphoreGive(g_stream_lock);}
 assert(!pet_seq_active(&g_cext_sequencer));assert(!CEXT_PENDING());
 xSemaphoreTake(g_stream_lock,0);assert(!cext_mix_step(pcm,false));xSemaphoreGive(g_stream_lock);
 pet_p4_audio_cext_set(false,NULL,false,0,0);
 pet_cext_audio_t old_snapshot={.enabled=true,.sample_pending=1,.samples_volume=100};
 miniapp_ext_audio_apply(&old_snapshot,NULL);assert(!atomic_load(&g_cext_enabled));
 miniapp_ext_audio_apply_snapshot(&old_snapshot,NULL,sample.generation);assert(!atomic_load(&g_cext_enabled));
 assert(pet_cmedia_load(0,0,&runtime));
 runtime.samples=true;runtime.sample_pending=1;runtime.samples_volume=100;runtime.media_generation=pet_cmedia_generation();
 pet_cext_audio_t real_snapshot={0};pet_cext_audio_take(&runtime,true,&real_snapshot);
 assert(real_snapshot.generation==pet_cmedia_generation());miniapp_ext_audio_apply(&real_snapshot,NULL);assert(g_cext_sample_queued==1);
 pet_cext_audio_t idle_snapshot={.enabled=true,.samples_volume=100};
 miniapp_ext_audio_apply_snapshot(&idle_snapshot,NULL,pet_cmedia_generation());assert(g_cext_sample_queued==1);
 pet_cmedia_unload();miniapp_ext_audio_apply_snapshot(&old_snapshot,NULL,sample.generation);assert(g_cext_sample_queued==1);
 // Deliberately hold media_mutex while another thread resets under stream_lock.
 // Its releases must finish without taking media_mutex (would otherwise ABBA).
 lock();pthread_t stopping;assert(!pthread_create(&stopping,NULL,audio_stop_thread,NULL));
 pthread_join(stopping,NULL);unlock();pet_cmedia_collect();
 vSemaphoreDelete(g_stream_lock);g_stream_lock=NULL;
}
static void test_publish_adapter(void){
 g_stream_lock=xSemaphoreCreateMutex();assert(pet_cmedia_load(0,0,&runtime));
 pet_p4_audio_cext_set(true,NULL,false,0,100);
 uint32_t generation=pet_cmedia_generation();miniapp_runtime_t next={.cext=runtime};
 assert(!miniapp_ext_publish(&next,1,0));assert(pet_cmedia_generation()==generation);assert(atomic_load(&g_cext_enabled));
 pet_cmedia_sample_lease_t old={0};assert(pet_cmedia_sample_pin(0,&old));
 pet_cext_publish_ticket_t ticket={0};assert(miniapp_ext_prepare(&next,0,0,&ticket));
 assert(pet_cmedia_generation()==generation&&atomic_load(&g_cext_enabled));assert(old.pcm[0]==1234);
 pet_cext_publish_ticket_t second={0};assert(!miniapp_ext_prepare(&next,0,0,&second));
 pet_cmedia_unload();assert(pet_cmedia_generation()==generation); // reservation cannot be invalidated
 assert(!pet_cmedia_load(0,0,&runtime));
 assert(miniapp_ext_abort(&ticket));assert(!ticket.media.serial&&pet_cmedia_generation()==generation);assert(atomic_load(&g_cext_enabled));
 assert(old.pcm[0]==1234); // catalog failure left old visible/audio/borrow intact
 assert(miniapp_ext_prepare(&next,0,0,&ticket));assert(miniapp_ext_commit(&ticket));
 assert(!miniapp_ext_commit(&ticket));assert(next.cext.media_generation==pet_cmedia_generation());assert(!atomic_load(&g_cext_enabled));
 assert(old.pcm[0]==1234);assert(pet_cmedia_unpin(old.token));
 assert(miniapp_ext_publish(NULL,0,0));pet_cmedia_collect();
 for(unsigned i=0;i<PET_CMEDIA_BANKS;i++)assert(!g_banks[i].pins&&!g_banks[i].samples&&!g_banks[i].clip);
 vSemaphoreDelete(g_stream_lock);g_stream_lock=NULL;
}
static void *clip_worker_thread(void *unused){
 (void)unused;portENTER_CRITICAL(&g_asset_prepare_mux);const uint8_t *bytes=g_clip_entry.bytes;portEXIT_CRITICAL(&g_asset_prepare_mux);
 pthread_mutex_lock(&gate);state=1;pthread_cond_broadcast(&cond);while(state<2)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
 // Resume the actual encoded-source read AFTER unload and cancelled retire attempts.
 for(unsigned i=0;i<10000;i++)assert(bytes[0]==0x65);
 portENTER_CRITICAL(&g_asset_prepare_mux);g_clip_entry.decoder_readers=0;g_h264_prefetch_lifetime.phase=PET_PREFETCH_EMPTY;decoder_finished=true;portEXIT_CRITICAL(&g_asset_prepare_mux);
 return NULL;
}
static void *store_holder(void *unused){
 (void)unused;lock();pthread_mutex_lock(&gate);state=1;pthread_cond_broadcast(&cond);
 while(state<2)pthread_cond_wait(&cond,&gate);
 pthread_mutex_unlock(&gate);
 unlock();
 return NULL;
}
static bool finish_commit,finish_result;
static void *finish_producer(void *arg){(void)arg;finish_result=finish_commit?miniapp_ext_commit(&preparing_ticket):miniapp_ext_abort(&preparing_ticket);return NULL;}
static void test_finish_wait_main_cycle(void){
 g_stream_lock=xSemaphoreCreateMutex();
 for(unsigned commit=0;commit<2;commit++){
  assert(pet_cmedia_load(0,0,&runtime));uint32_t gen=pet_cmedia_generation();
  pet_p4_audio_cext_set(true,NULL,false,0,100);preparing_next=(miniapp_runtime_t){.cext=runtime};
  preparing_ticket=(pet_cext_publish_ticket_t){0};assert(miniapp_ext_prepare(&preparing_next,0,0,&preparing_ticket));
  state=0;pthread_t writer,finisher;assert(!pthread_create(&writer,NULL,store_holder,NULL));
  pthread_mutex_lock(&gate);while(state<1)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
  finish_commit=commit;finish_result=false;assert(!pthread_create(&finisher,NULL,finish_producer,NULL));
  bool finishing=false;
  for(unsigned spin=0;spin<100000&&!finishing;spin++){
   ext_lock();finishing=g_ext_publish_phase==EXT_PUBLISH_FINISHING;ext_unlock();sched_yield();
  }assert(finishing);
  unsigned before=event_dispatches;g_runtime.cext.emit_pending=1;miniapp_ext_process(200,true);assert(event_dispatches==before+1);
  pet_cext_audio_t snapshot={.enabled=false,.generation=gen};miniapp_ext_audio_apply_snapshot(&snapshot,NULL,gen);
  assert(atomic_load(&g_cext_enabled));
  /* Competing completion cannot touch the in-flight ticket/store operation. */
  assert(!miniapp_ext_abort(&preparing_ticket));assert(!miniapp_ext_commit(&preparing_ticket));
  pthread_mutex_lock(&gate);assert(state==1);state=2;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);
  pthread_join(writer,NULL);pthread_join(finisher,NULL);assert(finish_result&&!g_ext_pending_publish);
  assert(g_ext_publish_phase==EXT_PUBLISH_IDLE&&!g_ext_media_transition);
  assert(atomic_load(&g_cext_enabled)==!commit);
  pet_p4_audio_cext_set(false,NULL,false,0,0);pet_cmedia_unload();
 }
 vSemaphoreDelete(g_stream_lock);g_stream_lock=NULL;
}
static void test_frame_try_while_writer(void){
 assert(pet_cmedia_load(0,0,&runtime));pet_cmedia_clip_lease_t clip={0};
 assert(pet_cmedia_clip_pin(0,&clip));assert(renderer_clip_entry(&clip,"clip:busy"));
 uint32_t gen=g_clip_lease.generation;decoder_finished=true;g_clip_entry.decoder_readers=0;g_h264_prefetch_lifetime.phase=PET_PREFETCH_EMPTY;
 state=0;pthread_t writer;assert(!pthread_create(&writer,NULL,store_holder,NULL));
 pthread_mutex_lock(&gate);while(state<1)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
 pet_cext_big_t meta={0};pet_cmedia_clip_lease_t c={0};pet_cmedia_sample_lease_t a={0};
 assert(!pet_cmedia_big_meta_try_at(0,gen,&meta));assert(!pet_cmedia_clip_pin_try_at(0,gen,&c));
 assert(!pet_cmedia_sample_pin_try_at(0,gen,&a));assert(!pet_cmedia_generation_try(gen));
 assert(!pet_cmedia_collect_try());
 /* Full production draw entry, not just its pin/retire helpers: the writer
  * cannot unlock until BOTH draws return. Keep a worker-owned source alive. */
 renderer_ext_media_snapshot(gen);pet_surface_node_t node={.w=160,.h=120};
 uint64_t token=g_clip_lease.token;unsigned fills=frame_fills;
 decoder_finished=false;g_clip_entry.decoder_readers=1;g_h264_prefetch_lifetime.phase=PET_PREFETCH_WORKING;
 renderer_ext_draw_clip(&node,1000);
 assert(frame_fills==fills+1&&g_clip_lease.token==token&&g_clip_entry.bytes[0]==0x65);
 /* Once physically idle the same busy-generation entry may retire without
  * taking the store mutex; reclamation still waits for a later collect. */
 decoder_finished=true;g_clip_entry.decoder_readers=0;g_h264_prefetch_lifetime.phase=PET_PREFETCH_EMPTY;
 renderer_ext_draw_clip(&node,1001);
 assert(frame_fills==fills+2&&!g_clip_lease.token&&!g_clip_entry.bytes);
 assert(renderer_ext_clip_retire());assert(!g_clip_lease.token);
 assert(!pet_cmedia_collect_try());
 pthread_mutex_lock(&gate);assert(state==1);state=2;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);pthread_join(writer,NULL);
 assert(pet_cmedia_collect_try());assert(pet_cmedia_generation_try(gen));
 assert(pet_cmedia_clip_pin_try_at(0,gen,&c));assert(pet_cmedia_unpin_deferred(c.token));assert(pet_cmedia_collect_try());
 assert(!pet_cmedia_clip_pin_try_at(0,gen+1,&c));assert(!c.token);
 renderer_ext_media_snapshot(gen+1);renderer_ext_draw_clip(&node,1002);assert(!g_clip_lease.token);
 renderer_ext_media_snapshot(0);renderer_ext_draw_clip(&node,1003);assert(!g_clip_lease.token);
 /* Correct snapshot still enters the normal pin/draw path after contention. */
 renderer_ext_media_snapshot(gen);renderer_ext_draw_clip(&node,1004);assert(g_clip_lease.token);
 assert(renderer_ext_clip_retire());assert(pet_cmedia_collect_try());
 pet_cmedia_unload();
}
static void test_clip_retirement_adapter(void){
 assert(pet_cmedia_load(0,0,&runtime));pet_cmedia_clip_lease_t clip={0};
 assert(pet_cmedia_clip_pin(0,&clip));assert(renderer_clip_entry(&clip,"clip:test"));assert(!clip.token);
 renderer_ext_media_snapshot(g_clip_lease.generation);assert(g_ext_snapshot_generation==pet_cmedia_generation());
 decoder_finished=false;g_h264_prefetch_lifetime.phase=PET_PREFETCH_WORKING;g_clip_entry.decoder_readers=1;
 state=0;pthread_t worker;assert(!pthread_create(&worker,NULL,clip_worker_thread,NULL));
 pthread_mutex_lock(&gate);while(state<1)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
 pet_cmedia_unload();assert(!renderer_ext_clip_retire());assert(g_clip_entry.bytes[0]==0x65);assert(g_clip_lease.token);
 // Logical cancellation alone does not establish native reader completion.
 decoder_finished=true;assert(!renderer_ext_clip_retire());assert(g_clip_lease.token);
 pthread_mutex_lock(&gate);state=2;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);pthread_join(worker,NULL);
 assert(renderer_ext_clip_retire());assert(!g_clip_lease.token&&!g_clip_entry.bytes);
 pet_surface_node_t node={.w=160,.h=120};renderer_ext_draw_clip(&node,0);assert(!g_clip_lease.token);
 assert(renderer_ext_clip_retire());
 assert(pet_cmedia_collect_try());
 for(unsigned i=0;i<PET_CMEDIA_BANKS;i++)assert(!g_banks[i].pins&&!g_banks[i].clip);
}
'''
    text = harness.read_text()
    text = text.replace('int main(void){', stubs + audio_adapter + clip_adapter + miniapp_adapter + publish_adapter + process_adapter + adapter_tests + '\nint main(void){')
    text = text.replace(' pet_cmedia_stats_t stats=', ' test_audio_adapter();test_publish_adapter();test_prepare_io_main_cycle();test_finish_wait_main_cycle();test_frame_try_while_writer();test_clip_retirement_adapter();\n pet_cmedia_stats_t stats=')
    harness.write_text(text)
    # stat is also a C tag; use a function-like macro to preserve struct stat.
    harness.write_text(harness.read_text().replace('#define stat mapped_stat', '#define stat(p,s) mapped_stat(p,s)'))
    binary=tmp_path/'leases'
    subprocess.run(['cc','-std=c11','-pthread','-fsanitize=address,undefined','-g','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-I',str(tmp_path),'-I',str(root/'main'),'-I',str(root/'tests/vendor/cjson'),str(harness),*[str(root/'main'/f) for f in ('pet_p4_component_ext.c','pet_p4_component_events.c','pet_p4_sfx_core.c')],str(root/'tests/vendor/cjson/cJSON.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True,timeout=10)


def test_frame_tile_try_admission_and_writer_lock_with_actual_broker(tmp_path):
    """Compile the ESP path with real store/resource core; no device or flash."""
    root = Path(__file__).resolve().parents[1]
    (tmp_path/'freertos').mkdir()
    (tmp_path/'esp_heap_caps.h').write_text('#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_malloc(n,c) malloc(n)\n')
    (tmp_path/'esp_log.h').write_text('#define ESP_LOGW(tag,...) ((void)(tag))\n')
    (tmp_path/'freertos/FreeRTOS.h').write_text('#include <assert.h>\n#define configASSERT(x) assert(x)\n#define pdTRUE 1\n#define portMAX_DELAY 0xffffffffu\n')
    (tmp_path/'freertos/semphr.h').write_text('''#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){SemaphoreHandle_t p=malloc(sizeof(*p));if(p)pthread_mutex_init(p,NULL);return p;}
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned t){return (t?pthread_mutex_lock(p):pthread_mutex_trylock(p))==0;}
static inline int xSemaphoreGive(SemaphoreHandle_t p){return pthread_mutex_unlock(p)==0;}
static inline void vSemaphoreDelete(SemaphoreHandle_t p){pthread_mutex_destroy(p);free(p);}
''')
    source = tmp_path/'admission.c'
    source.write_text(r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_resources_device.h"
static pet_resource_broker_t broker;
static unsigned opens;
static FILE *count_open(const char *p,const char *m){opens++;return fopen(p,m);}
#define fopen count_open
#define ESP_PLATFORM 1
#include "pet_p4_component_media.c"
#undef fopen
pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t *o){return pet_resources_owner_open(&broker,m,o);}
pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t n,pet_resource_lease_t *l){return pet_resources_try_acquire(&broker,o,k,n,l);}
bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
void pet_resources_device_revoke(pet_resource_owner_t o){(void)pet_resources_owner_revoke(&broker,o);}
pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){return pet_resources_finish(&broker,l);}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static int held;
static void *writer(void *arg){(void)arg;lock();pthread_mutex_lock(&gate);held=1;pthread_cond_broadcast(&ready);
 while(held==1)pthread_cond_wait(&ready,&gate);pthread_mutex_unlock(&gate);unlock();return NULL;}
int main(void){
 lock();g_generation=g_active.generation=1;g_active.big_count=1;
 g_active.big_table[0]=calloc(1,28);assert(g_active.big_table[0]);
 g_active.big_table[0][14]=1;g_active.big_table[0][20]=28;g_active.big_table[0][24]=4;
 g_active.big_meta[0].bytes=32;strcpy(g_active.big_path[0],"tile.bin");unlock();
 FILE *f=fopen("tile.bin","wb");assert(f);unsigned char file[32]={0};memcpy(file+28,"tile",4);assert(fwrite(file,1,32,f)==32);fclose(f);
 uint8_t data[4];size_t bytes=99;pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&owner)==PET_RESOURCE_OK);
 assert(pet_resources_device_acquire(owner,PET_RESOURCE_STORAGE_WORK,1,&lease)==PET_RESOURCE_OK);
 assert(!pet_cmedia_big_tile_try_at(0,0,1,data,4,&bytes)&&!bytes&&!opens);
 (void)pet_resources_device_finish(lease);pet_resources_device_revoke(owner);
 pthread_t worker;assert(!pthread_create(&worker,NULL,writer,NULL));
 pthread_mutex_lock(&gate);while(!held)pthread_cond_wait(&ready,&gate);pthread_mutex_unlock(&gate);
 for(unsigned i=0;i<100;i++)assert(!pet_cmedia_big_tile_try_at(0,0,1,data,4,&bytes)&&!bytes&&!opens);
 assert(!pet_cmedia_collect_try());
 pthread_mutex_lock(&gate);assert(held==1);held=2;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&gate);pthread_join(worker,NULL);
 /* No owner/lease leak from denied attempts: admit, read, close, then admit again. */
 for(unsigned i=0;i<3;i++){assert(pet_cmedia_big_tile_try_at(0,0,1,data,4,&bytes));assert(bytes==4&&!memcmp(data,"tile",4));}
 assert(opens==3);assert(!pet_cmedia_big_tile_try_at(0,0,2,data,4,&bytes)&&!bytes&&opens==3);
 assert(!pet_cmedia_big_tile_try_at(0,0,0,data,4,&bytes)&&!bytes&&opens==3);
 assert(!pet_cmedia_big_tile_try_at(0,0,1,data,3,&bytes)&&!bytes&&opens==3);
 pet_resource_usage_t usage={0};pet_resources_usage(&broker,&usage);
 assert(pet_cmedia_collect_try());pet_cmedia_unload();return 0;
}
''')
    binary = tmp_path/'admission'
    subprocess.run(['cc','-std=c11','-pthread','-fsanitize=address,undefined','-g','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-Wno-error=deprecated-declarations','-I',str(tmp_path),'-I',str(root/'main'),'-I',str(root/'tests/vendor/cjson'),str(source),
                    *[str(root/'main'/f) for f in ('pet_p4_component_ext.c','pet_p4_component_events.c','pet_p4_sfx_core.c','pet_p4_resources.c')],
                    str(root/'tests/vendor/cjson/cJSON.c'),'-o',str(binary)], check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True,timeout=10)
