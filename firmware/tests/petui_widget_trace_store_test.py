"""Real store/selection + real numeric trace, with deterministic task/IO faults."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_real_store_and_selection_trace_lanes_failures_and_idempotence(tmp_path):
    stubs = {
        'esp_attr.h': '#pragma once\n#define EXT_RAM_BSS_ATTR\n',
        'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
        'esp_log.h': '#pragma once\nvoid trace_log(const char*,...);\n#define ESP_LOGI(tag,...) trace_log(__VA_ARGS__)\n',
        'freertos/FreeRTOS.h': '''#pragma once
#include <assert.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern unsigned critical_depth;
#define portENTER_CRITICAL(m) do{(void)(m);assert(!critical_depth);critical_depth++;}while(0)
#define portEXIT_CRITICAL(m) do{(void)(m);assert(critical_depth==1);critical_depth--;}while(0)
#define portMAX_DELAY 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
''',
        'freertos/task.h': '''#pragma once
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
#define xTaskCreate(fn,name,stack,arg,priority,out) ((void)(fn),(void)(name),(void)(stack),(void)(arg),(void)(priority),*(out)=(void*)2,1)
#define vTaskDelay(n) ((void)(n))
''',
        'freertos/semphr.h': '''#pragma once
typedef int SemaphoreHandle_t;
int store_take(void);void store_give(void);
#define xSemaphoreCreateMutex() 1
#define xSemaphoreTake(a,b) ((void)(a),(void)(b),store_take())
#define xSemaphoreGive(a) ((void)(a),store_give())
''',
        'mbedtls/sha256.h': '''#pragma once
#include <string.h>
static int mbedtls_sha256(const unsigned char*p,size_t n,unsigned char*out,int b){(void)b;memset(out,0,32);for(size_t i=0;i<n;i++)out[i%32]^=p[i];return 0;}
''',
        'nvs.h': '''#pragma once
#include <stddef.h>
typedef int esp_err_t;typedef int nvs_handle_t;
enum{ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_ERROR=2,NVS_READONLY=3,NVS_READWRITE=4};
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);void nvs_close(nvs_handle_t);
'''
    }
    for name, body in stubs.items():
        path = tmp_path / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text(body)
    store = (ROOT / 'main/pet_p4_app_store.c').read_text().replace('#include "pet_p4_audio.h"', '')
    source = r'''
#define PET_WIDGET_TRACE_TEST 1
#define PET_FS_ACTIVITY_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "pet_p4_app_store.h"
unsigned critical_depth;
static unsigned store_locked,logs;
static uint64_t clock_us,protocol_seq;
static uintptr_t task_id=1;
static bool audio,playback;
static int failure;
int64_t esp_timer_get_time(void){errno=ERANGE;return (int64_t)clock_us;}
void *xTaskGetCurrentTaskHandle(void){return (void*)task_id;}
int store_take(void){assert(!critical_depth&&!store_locked);store_locked=1;return 1;}
void store_give(void){assert(!critical_depth&&store_locked);store_locked=0;}
void trace_log(const char *fmt,...){
  assert(!critical_depth&&!store_locked);char line[2048];va_list ap;va_start(ap,fmt);
  int n=vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);assert(n>0&&(size_t)n<sizeof(line));
  assert(!strstr(line,"garden")&&!strstr(line,"/spiffs")&&!strstr(line,"secret"));logs++;errno=EIO;
}
static bool pet_p4_audio_stream_playing(void){return audio;}
static bool pet_p4_audio_music_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_active(void){return false;}
static bool pet_p4_audio_playback_active(void){return playback;}
#include "pet_p4_widget_trace.c"
#include "pet_p4_fs_activity.c"
static void at_stage(pet_widget_trace_stage_t stage){
  assert(!critical_depth);unsigned lane=task_id==2?1:0;
  if(task_id==2){uint64_t epoch;assert(!pet_fs_activity_idle_epoch(&epoch));}
  assert(trace_lanes[lane].active&&trace_lanes[lane].stage==stage);
  if(task_id==2&&protocol_seq){
    pet_widget_trace_snapshot_t snap;pet_widget_trace_snapshot(&snap);
    assert(snap.lanes[0].seq==protocol_seq&&snap.lanes[0].active&&snap.lanes[0].current_stage==PET_WTRACE_CLEANUP);
  }
  clock_us+=3;
}
static FILE *fault_open(const char*p,const char*m){at_stage(PET_WTRACE_FS_OPEN);if(failure==1){errno=EACCES;return NULL;}return fopen(p,m);}
static size_t fault_read(void*p,size_t n,size_t count,FILE*f){at_stage(PET_WTRACE_FS_DATA);return fread(p,n,count,f);}
static unsigned seeks,tells;
static int fault_seek(FILE*f,long offset,int whence){at_stage(PET_WTRACE_FS_DATA);seeks++;return fseek(f,offset,whence);}
static long fault_tell(FILE*f){at_stage(PET_WTRACE_FS_DATA);tells++;return ftell(f);}
static size_t fault_write(const void*p,size_t n,size_t count,FILE*f){at_stage(PET_WTRACE_FS_DATA);if(failure==2){errno=ENOSPC;return 0;}return fwrite(p,n,count,f);}
static int fault_flush(FILE*f){at_stage(PET_WTRACE_FS_DATA);return fflush(f);}
static int fault_sync(int fd){at_stage(PET_WTRACE_FS_DATA);return fsync(fd);}
static int fault_close(FILE*f){at_stage(PET_WTRACE_FS_CLOSE);int result=fclose(f);if(failure==3){errno=EIO;return EOF;}return result;}
static int fault_unlink(const char*p){at_stage(PET_WTRACE_FS_REMOVE);if(failure==4){errno=EPERM;return -1;}return unlink(p);}
#define fopen fault_open
#define fread fault_read
#define fseek fault_seek
#define ftell fault_tell
#define fwrite fault_write
#define fflush fault_flush
#define fsync fault_sync
#define fclose fault_close
#define unlink fault_unlink
/* STORE */
#undef fopen
#undef fread
#undef fseek
#undef ftell
#undef fwrite
#undef fflush
#undef fsync
#undef fclose
#undef unlink
#include "pet_p4_selection.c"
static selection_t disk,pending;
static bool exists,nvs_open_error,nvs_set_error,nvs_commit_error;
static unsigned writes,commits;
esp_err_t nvs_open(const char*name,int mode,nvs_handle_t*out){
  at_stage(PET_WTRACE_NVS);assert(!strcmp(name,"p4_widgets"));*out=1;
  if(nvs_open_error)return NVS_ERROR;
  if(mode==NVS_READONLY&&!exists)return ESP_ERR_NVS_NOT_FOUND;
  return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*out,size_t*n){at_stage(PET_WTRACE_NVS);assert(h==1&&!strcmp(k,"selection")&&*n==sizeof(disk));memcpy(out,&disk,*n);return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*value,size_t n){at_stage(PET_WTRACE_NVS);assert(h==1&&!strcmp(k,"selection")&&n==sizeof(pending));writes++;if(nvs_set_error)return NVS_ERROR;memcpy(&pending,value,n);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){at_stage(PET_WTRACE_NVS);assert(h==1);commits++;if(nvs_commit_error)return NVS_ERROR;disk=pending;exists=true;return ESP_OK;}
void nvs_close(nvs_handle_t h){at_stage(PET_WTRACE_NVS);assert(h==1);}
static void finished(unsigned lane,bool ok){assert(!trace_lanes[lane].active&&!trace_lanes[lane].depth&&!trace_lanes[lane].bad_scopes&&trace_lanes[lane].ok==ok);assert(!critical_depth&&!store_locked);}
int main(void){
  pet_fs_activity_enable_observation();
  assert(pet_app_store_init());assert(pet_app_store_update("garden","{\"count\":7}"));
  pet_widget_trace_operation_t op=pet_widget_trace_begin(PET_WTRACE_DELETE);
  pet_widget_trace_scope_t protocol=pet_widget_trace_scope_begin(PET_WTRACE_CLEANUP);protocol_seq=op.seq;
  task_id=2;clock_us=31000000;audio=true;unsigned before=logs;flush_one();
  assert(logs==before&&!trace_lanes[1].active&&!trace_lanes[1].last_seq);
  audio=false;playback=true;uint64_t before_audio,after_audio;
  assert(pet_fs_activity_idle_epoch(&before_audio));flush_one();
  assert(logs==before&&!trace_lanes[1].active&&!trace_lanes[1].last_seq&&saves[0].dirty);
  assert(pet_fs_activity_idle_epoch(&after_audio)&&after_audio==before_audio);
  playback=false;failure=1;flush_one();finished(1,false);
  assert(logs==before+2&&saves[0].dirty&&saves[0].file_slot==0);
  assert(trace_lanes[1].stages[PET_WTRACE_SAVE_WRITE].count==1&&trace_lanes[1].stages[PET_WTRACE_FS_OPEN].count==1);
  assert(!trace_lanes[1].stages[PET_WTRACE_FS_DATA].count&&!trace_lanes[1].stages[PET_WTRACE_FS_CLOSE].count);
  assert(trace_lanes[0].active&&trace_lanes[0].seq==protocol_seq&&trace_lanes[0].stage==PET_WTRACE_CLEANUP);
  failure=0;flush_one();finished(1,true);assert(logs==before+4&&!saves[0].dirty&&saves[0].file_slot==1);
  assert(trace_lanes[1].stages[PET_WTRACE_FS_OPEN].count==1&&trace_lanes[1].stages[PET_WTRACE_FS_DATA].count==1&&trace_lanes[1].stages[PET_WTRACE_FS_CLOSE].count==1);
  before=logs;flush_one();assert(logs==before); /* No ready save, no operation. */
  task_id=1;pet_widget_trace_scope_end(&protocol);pet_widget_trace_end(&op,true);protocol_seq=0;finished(0,true);

  char out[PET_APP_SAVE_BYTES];memset(saves,0,sizeof(saves));
  op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":7}"));
  assert(seeks==4&&tells==2); /* Both extent checks stay inside FS_DATA. */
  assert(trace_lanes[0].stages[PET_WTRACE_SAVE_READ].count==3&&trace_lanes[0].stages[PET_WTRACE_FS_OPEN].count==4);
  assert(trace_lanes[0].stages[PET_WTRACE_FS_DATA].count==1&&trace_lanes[0].stages[PET_WTRACE_FS_CLOSE].count==1);
  pet_widget_trace_end(&op,true);finished(0,true);
  for(int mode=2;mode<=3;mode++){
    assert(pet_app_store_update("garden",mode==2?"{\"count\":8}":"{\"count\":9}"));
    clock_us+=31000000;task_id=2;failure=mode;flush_one();finished(1,false);assert(saves[0].dirty);
    assert(trace_lanes[1].stages[PET_WTRACE_FS_DATA].count==1&&trace_lanes[1].stages[PET_WTRACE_FS_CLOSE].count==1);
    failure=0;flush_one();finished(1,true);task_id=1;
  }
  pet_app_store_forget("garden");task_id=2;failure=4;flush_one();finished(1,false);assert(saves[0].dirty);
  assert(trace_lanes[1].stages[PET_WTRACE_FS_REMOVE].count==1); /* target namespace failure stops before committing epoch */
  failure=0;flush_one();finished(1,true);assert(!saves[0].id[0]);task_id=1;


  // A torn FIRST epoch metadata write cannot permanently poison legacy saves.
  op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);
  assert(pet_app_store_update("epoch-fault","{\"count\":42}"));pet_widget_trace_end(&op,true);
  task_id=2;clock_us+=31000000;failure=0;flush_one();finished(1,true);task_id=1;
  op=pet_widget_trace_begin(PET_WTRACE_DELETE);save_io_t delete_io={0};assert(save_io_begin(&delete_io,NULL));
  failure=2;assert(!pet_app_store_forget_with_lease("epoch-fault",delete_io.lease));save_io_end(&delete_io);
  pet_widget_trace_end(&op,false);finished(0,false);failure=0;memset(saves,0,sizeof(saves));
  op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(pet_app_store_read("epoch-fault",out)&&!strcmp(out,"{\"count\":42}"));pet_widget_trace_end(&op,true);finished(0,true);
  op=pet_widget_trace_begin(PET_WTRACE_DELETE);assert(save_io_begin(&delete_io,NULL));
  assert(pet_app_store_forget_with_lease("epoch-fault",delete_io.lease));save_io_end(&delete_io);pet_widget_trace_end(&op,true);finished(0,true);
  memset(saves,0,sizeof(saves));op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(pet_app_store_load("epoch-fault",out)==PET_APP_STORE_EMPTY);pet_widget_trace_end(&op,true);finished(0,true);

  op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(pet_selection_write(10,"first"));
  assert(trace_lanes[0].stages[PET_WTRACE_NVS].count==2&&writes==1&&commits==1);
  pet_widget_trace_end(&op,true);finished(0,true);
  op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(pet_selection_write(10,"first"));
  assert(trace_lanes[0].stages[PET_WTRACE_NVS].count==1&&writes==1&&commits==1);
  pet_widget_trace_end(&op,true);finished(0,true);
  for(int mode=0;mode<3;mode++){
    nvs_open_error=mode==0;nvs_set_error=mode==1;nvs_commit_error=mode==2;
    op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(!pet_selection_write(10,"second"));
    assert(!trace_lanes[0].depth&&trace_lanes[0].stages[PET_WTRACE_NVS].count==2);
    pet_widget_trace_end(&op,false);finished(0,false);assert(!strcmp(disk.id,"first"));
  }
  nvs_open_error=nvs_set_error=nvs_commit_error=false;
  op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(!pet_selection_write(10,"../bad"));
  assert(!trace_lanes[0].stages[PET_WTRACE_NVS].count);pet_widget_trace_end(&op,false);finished(0,false);
  assert(!critical_depth&&!store_locked);return 0;
}
'''.replace('/* STORE */', store)
    path = tmp_path / 'trace_store.c'
    binary = tmp_path / 'trace_store'
    path.write_text(source)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                    '-DPET_APP_SAVE_ROOT="."', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(tmp_path),
                    '-I', str(ROOT / 'main'), str(path), str(ROOT/'main/pet_p4_resources.c'),
                    str(ROOT/'main/pet_p4_resources_device.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], cwd=tmp_path, check=True)
