"""Production numeric trace, real protocol handler and file helpers with injected IO.
No device or SDK mutation; clocks, task identity and failures are local adapters.
"""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT = Path(__file__).resolve().parents[1]

def test_production_widget_trace_protocol_early_returns_and_file_boundaries(tmp_path):
    check_trace(tmp_path,False)

def test_spare_open_trace_preserves_protocol_errno_lifo_and_bounded_numeric_reports(tmp_path):
    check_trace(tmp_path,True)

def check_trace(tmp_path,spare_trace):
    stubs = {
        "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "esp_log.h": "#pragma once\nvoid trace_log(const char*,...);\n#define ESP_LOGI(tag,...) trace_log(__VA_ARGS__)\n",
        "freertos/FreeRTOS.h": """#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
extern unsigned critical_depth;
#define portENTER_CRITICAL(m) do { assert(!critical_depth);pthread_mutex_lock(m);critical_depth++; } while(0)
#define portEXIT_CRITICAL(m) do { critical_depth--;pthread_mutex_unlock(m); } while(0)
#define pdMS_TO_TICKS(n) (n)
""",
        "freertos/task.h": "#pragma once\ntypedef void *TaskHandle_t;\nTaskHandle_t xTaskGetCurrentTaskHandle(void);\nvoid vTaskDelay(unsigned);\n",
    }
    for name, text in stubs.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    protocol=(ROOT/"main/pet_p4_protocol.c").read_text()
    miniapp=(ROOT/"main/pet_p4_miniapp.c").read_text()
    helpers="\n".join(function(miniapp,name) for name in
        ("read_file_kind","read_file","read_binary_file","write_file_atomic","miniapp_checksum","remove_file_if_present"))
    handlers="\n".join(function(protocol,name) for name in
        ("send_protocol_ack","send_widget_ack","handle_widget_topic"))
    harness=r'''
#define PET_WIDGET_TRACE_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include "pet_p4_json_source.h"
#include "pet_p4_file_read.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
unsigned critical_depth;
static uint64_t now;
static uintptr_t current_task=1;
static unsigned logs, ack_count;
static size_t longest_log;
static bool handler_ok=true,audio_busy;
static bool storage_work_current(void){return true;}
int64_t esp_timer_get_time(void){errno=ERANGE;return (int64_t)now;}
void *xTaskGetCurrentTaskHandle(void){return (void *)current_task;}
void vTaskDelay(unsigned ticks){assert(!critical_depth);now+=1000*ticks;}
void trace_log(const char *fmt,...){
 assert(!critical_depth);char line[2048];va_list ap;va_start(ap,fmt);
 int n=vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
 assert(n>0&&(size_t)n<sizeof(line));if((size_t)n>longest_log)longest_log=(size_t)n;
 assert(!strstr(line,"SECRET")&&!strstr(line,"private")&&!strstr(line,"/spiffs"));
 logs++;errno=EIO;
}
#include "pet_p4_widget_trace.c"
typedef void (*pet_p4_send_line_fn)(const char *,void *);
typedef struct {char screen_page[16];unsigned last_update_ms;int stats;} pet_p4_runtime_state_t;
static const char *json_string(const cJSON *p,const char *key){
 const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);return cJSON_IsString(v)?v->valuestring:"";
}
static void copy_text(char *out,size_t size,const char *value){snprintf(out,size,"%s",value);}
static void send_topic_payload(pet_p4_send_line_fn send,void *ctx,const char *topic,cJSON *p){
 (void)send;(void)ctx;(void)topic;assert(trace_lanes[0].active&&trace_lanes[0].stage==PET_WTRACE_ACK);
 ack_count++;now+=13;cJSON_Delete(p);
}
static bool action(const cJSON *p,char *error,size_t size){
 (void)p;(void)error;(void)size;PET_WIDGET_TRACE_SCOPE(trace, PET_WTRACE_PARSE);now+=7;return handler_ok;
}
/* RPC field validation is exercised against the real include by
 * p4_background_protocol_test; this test isolates the outer trace/ACK lane. */
static bool handle_background_program_topic(pet_p4_runtime_state_t *s,const char *t,const cJSON *p,pet_p4_send_line_fn f,void *c){
 (void)s;(void)t;(void)f;(void)c;return action(p,NULL,0);
}
static bool pet_p4_miniapp_install_begin(const cJSON*p,char*e,size_t s){return action(p,e,s);}
static bool pet_p4_miniapp_install_chunk(const cJSON*p,char*e,size_t s){return action(p,e,s);}
static bool pet_p4_miniapp_install_commit(const cJSON*p,char*e,size_t s){return action(p,e,s);}
static bool pet_p4_miniapp_resource_repair_active(void){return false;}
static void pet_p4_miniapp_sync_stats(int *stats){*stats=3;}
static bool send_widget_inventory(pet_p4_runtime_state_t*s,const cJSON*p,pet_p4_send_line_fn f,void*c){
 (void)s;(void)p;(void)f;(void)c;return true;
}
static bool pet_p4_audio_active(void){return audio_busy;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
/* Selection has its own real parser/core regression; this trace harness must
 * never invoke it while exercising the legacy install/activation lanes. */
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
typedef struct {char widget_id[48];} pet_p4_miniapp_catalog_entry_t;
static bool pet_p4_miniapp_catalog_recovery_blocked(void){assert(false);return true;}
static size_t pet_p4_miniapp_catalog_count(void){assert(false);return 0;}
static bool pet_p4_miniapp_catalog_get(size_t i,pet_p4_miniapp_catalog_entry_t*out){(void)i;(void)out;assert(false);return false;}
static bool pet_p4_miniapp_catalog_select_durable(size_t i){(void)i;assert(false);return false;}
static bool pet_p4_miniapp_catalog_activate_id(const char*id){(void)id;return action(NULL,NULL,0);}
static bool pet_p4_miniapp_remove_preserving_save(const char*id,char*e,size_t s){(void)id;return action(NULL,e,s);}
static bool pet_p4_miniapp_remove(const char*id,char*e,size_t s){(void)id;return action(NULL,e,s);}
static bool send_catalog_admin(pet_p4_runtime_state_t*s,const char*t,const cJSON*p,pet_p4_send_line_fn f,void*c){
 (void)s;(void)t;(void)p;(void)f;(void)c;assert(false);return false;
}
/* PROTOCOL */
static int failure;
static char g_storage_failure[160];
static FILE *fault_open(const char *p,const char *mode){assert(!critical_depth);now+=2;if(failure==1){errno=EACCES;return NULL;}return fopen(p,mode);}
static size_t fault_read(void*p,size_t n,size_t c,FILE*f){assert(!critical_depth);now+=3;return fread(p,n,c,f);}
static size_t fault_write(const void*p,size_t n,size_t c,FILE*f){assert(!critical_depth);now+=5;if(failure==2){errno=ENOSPC;return 0;}return fwrite(p,n,c,f);}
static int fault_flush(FILE*f){assert(!critical_depth);now+=7;if(failure==3){errno=ENOSPC;return EOF;}return fflush(f);}
static int fault_close(FILE*f){assert(!critical_depth);now+=11;int r=fclose(f);if(failure==4){errno=EIO;return EOF;}return r;}
static int fault_remove(const char*p){assert(!critical_depth);now+=17;if(failure==5){errno=EPERM;return -1;}return remove(p);}
static int fault_rename(const char*p,const char*q){assert(!critical_depth);now+=19;if(failure==6){errno=EPERM;return -1;}return rename(p,q);}
#define fopen fault_open
#define fread fault_read
#define fwrite fault_write
#define fflush fault_flush
#define fclose fault_close
#define remove fault_remove
#define rename fault_rename
/* FILE_HELPERS */
#undef fopen
#undef fread
#undef fwrite
#undef fflush
#undef fclose
#undef remove
#undef rename
static bool early_scope(void){
 PET_WIDGET_TRACE_SCOPE(trace,PET_WTRACE_PARSE);now+=9;return false;
}
static void test_core(void){
 errno=EACCES;pet_widget_trace_operation_t op=pet_widget_trace_begin(PET_WTRACE_COMMIT);assert(errno==EACCES&&op.owned);
 uint64_t seq=op.seq;PET_WIDGET_TRACE_SCOPE(outer,PET_WTRACE_CATALOG_WRITE);now+=5;
 assert(!early_scope());assert(trace_lanes[0].stage==PET_WTRACE_CATALOG_WRITE&&trace_lanes[0].depth==1);
 current_task=2;pet_widget_trace_operation_t save=pet_widget_trace_begin(PET_WTRACE_SAVE_FLUSH);assert(save.owned);
 {PET_WIDGET_TRACE_SCOPE(write,PET_WTRACE_SAVE_WRITE);now+=10;}
 pet_widget_trace_snapshot_t snap;pet_widget_trace_snapshot(&snap);
 assert(snap.lanes[0].seq==seq&&snap.lanes[0].current_stage==PET_WTRACE_CATALOG_WRITE);
 assert(snap.lanes[1].seq==save.seq&&snap.lanes[1].active);
 pet_widget_trace_end(&save,false);assert(trace_lanes[0].active);
 current_task=3;assert(!pet_widget_trace_scope_begin(PET_WTRACE_NVS).active);
 pet_widget_trace_operation_t denied=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);assert(!denied.owned);
 pet_widget_trace_end(&denied,true);assert(trace_lanes[0].active&&trace_lanes[0].seq==seq);
 current_task=1;now+=3;pet_widget_trace_scope_end(&outer);assert(trace_lanes[0].stages[PET_WTRACE_CATALOG_WRITE].sum_us==27);
 assert(trace_lanes[0].stages[PET_WTRACE_PARSE].sum_us==9&&trace_lanes[1].stages[PET_WTRACE_SAVE_WRITE].sum_us==10);
 errno=EACCES;pet_widget_trace_end(&op,true);assert(errno==EACCES&&logs==(PET_P4_SPARE_OPEN_TRACE?6:4));
 unsigned before=logs;pet_widget_trace_end(&op,false);assert(logs==before);
 pet_widget_trace_snapshot(&snap);assert(!snap.lanes[0].active&&snap.lanes[0].last_completed_seq==seq);
 op=pet_widget_trace_begin(PET_WTRACE_DELETE);pet_widget_trace_scope_t scopes[9];
 for(unsigned i=0;i<9;i++)scopes[i]=pet_widget_trace_scope_begin(PET_WTRACE_CHECKSUM);
 assert(!scopes[8].active&&trace_lanes[0].bad_scopes==1);
 for(int i=8;i>=0;i--)pet_widget_trace_scope_end(&scopes[i]);
 assert(!trace_lanes[0].depth);pet_widget_trace_end(&op,false);
 op=pet_widget_trace_begin(PET_WTRACE_DELETE);pet_widget_trace_scope_t s=pet_widget_trace_scope_begin(PET_WTRACE_CHECKSUM);
 trace_lanes[0].stages[PET_WTRACE_CHECKSUM].sum_us=UINT64_MAX-1;
 trace_lanes[0].stages[PET_WTRACE_CHECKSUM].count=UINT32_MAX;now+=3;pet_widget_trace_scope_end(&s);
 assert(trace_lanes[0].stages[PET_WTRACE_CHECKSUM].sum_us==UINT64_MAX&&trace_lanes[0].stages[PET_WTRACE_CHECKSUM].count==UINT32_MAX);
 pet_widget_trace_operation_t stale=op;pet_widget_trace_end(&op,true);
 op=pet_widget_trace_begin(PET_WTRACE_DELETE);pet_widget_trace_end(&stale,false);assert(trace_lanes[0].active);pet_widget_trace_end(&op,true);
 {PET_WIDGET_TRACE_OPERATION(early,PET_WTRACE_ACTIVATE);assert(early.owned);assert(!early_scope());}
 assert(!trace_lanes[0].active&&!trace_lanes[0].ok&&!trace_lanes[0].depth);
}
static void test_protocol(void){
 const char *topics[]={"widget/commit","widget/delete","widget/activate"};
 for(unsigned i=0;i<3;i++)for(unsigned success=0;success<2;success++){
  cJSON*p=cJSON_Parse(i==2?"{\"widgetId\":\"SECRET-private\",\"show\":true}":"{\"widgetId\":\"SECRET-private\",\"transferId\":\"t\"}");
  pet_p4_runtime_state_t state={0};handler_ok=success;unsigned old=logs;
  assert(handle_widget_topic(&state,topics[i],p,NULL,NULL)==(bool)success);
  assert(logs==old+(PET_P4_SPARE_OPEN_TRACE?3:2)&&!trace_lanes[0].active&&!trace_lanes[0].depth&&!trace_lanes[0].bad_scopes);
  assert(trace_lanes[0].stages[PET_WTRACE_ACK].count==1&&trace_lanes[0].stages[PET_WTRACE_ACK].sum_us==13);
  assert(trace_lanes[0].stages[PET_WTRACE_PARSE].count==1);
  cJSON_Delete(p);
 }
 for(unsigned invalid=0;invalid<3;invalid++){
  cJSON*p=cJSON_Parse(invalid==0?"{\"show\":7}":invalid==1?"{\"show\":true}":"{}");
  pet_p4_runtime_state_t state={0};audio_busy=invalid==1;
  assert(!handle_widget_topic(&state,invalid==2?"widget/delete":"widget/activate",p,NULL,NULL));
  assert(!trace_lanes[0].active&&!trace_lanes[0].ok&&!trace_lanes[0].depth);
  assert(trace_lanes[0].stages[PET_WTRACE_ACK].count==1&&!trace_lanes[0].stages[PET_WTRACE_PARSE].count);
  cJSON_Delete(p);
 }
 audio_busy=false;handler_ok=true;
}
static void test_files(void){
 for(failure=0;failure<=6;failure++){
  remove("private.tmp");remove("private.dat");
  pet_widget_trace_operation_t op=pet_widget_trace_begin(PET_WTRACE_COMMIT);
  bool result=write_file_atomic("private.tmp","private.dat","abc",3);
  assert(result==(failure==0));assert(!trace_lanes[0].depth&&!trace_lanes[0].bad_scopes);
  assert(trace_lanes[0].stages[PET_WTRACE_FS_OPEN].count==1);
  if(failure!=1)assert(trace_lanes[0].stages[PET_WTRACE_FS_CLOSE].count==1&&trace_lanes[0].stages[PET_WTRACE_FS_CLOSE].sum_us==11);
  pet_widget_trace_end(&op,result);
 }
 failure=0;FILE*f=fopen("private.dat","wb");assert(f);fwrite("abc",1,3,f);fclose(f);
 pet_widget_trace_operation_t op=pet_widget_trace_begin(PET_WTRACE_ACTIVATE);char data[8];size_t size;
 assert(read_file("private.dat",data,sizeof(data))&&!strcmp(data,"abc"));
 assert(read_binary_file("private.dat",(uint8_t*)data,sizeof(data),&size)&&size==3);
 assert(miniapp_checksum("abc",3)==0x1a47e90bU);
 assert(remove_file_if_present("private.dat"));assert(remove_file_if_present("private.dat"));
 assert(trace_lanes[0].stages[PET_WTRACE_CLEANUP].count==2&&trace_lanes[0].stages[PET_WTRACE_CLEANUP].sum_us==34&&trace_lanes[0].stages[PET_WTRACE_CLEANUP].max_us==17);
 assert(trace_lanes[0].stages[PET_WTRACE_FS_DATA].count==2&&trace_lanes[0].stages[PET_WTRACE_FS_DATA].sum_us==6);
 pet_widget_trace_end(&op,true);remove("private.tmp");remove("private.dat");
}
static void test_open_purpose(void){
#if PET_P4_SPARE_OPEN_TRACE
 unsigned old=logs;
 pet_widget_trace_operation_t op=pet_widget_trace_begin(PET_WTRACE_COMMIT);
 PET_WIDGET_TRACE_SCOPE(open,PET_WTRACE_FS_OPEN);
 PET_WIDGET_TRACE_SCOPE(update,PET_WTRACE_SPARE_OPEN_UPDATE);now+=10;
 {PET_WIDGET_TRACE_SCOPE(create,PET_WTRACE_SPARE_OPEN_CREATE);now+=20;}
 now+=30;errno=ENOENT;pet_widget_trace_scope_end(&update);pet_widget_trace_scope_end(&open);
 assert(errno==ENOENT&&!trace_lanes[0].depth&&!trace_lanes[0].bad_scopes);
 assert(trace_lanes[0].stages[21].count==1&&trace_lanes[0].stages[21].sum_us==60);
 assert(trace_lanes[0].stages[22].count==1&&trace_lanes[0].stages[22].sum_us==20);
 assert(trace_lanes[0].stages[PET_WTRACE_FS_OPEN].sum_us==60);
 pet_widget_trace_end(&op,true);assert(errno==ENOENT&&logs==old+3);
#endif
}
int main(void){test_core();test_protocol();test_files();test_open_purpose();assert(longest_log<1024);return 0;}
'''
    harness=harness.replace("/* PROTOCOL */",handlers).replace("/* FILE_HELPERS */",helpers)
    source=tmp_path/"trace_test.c";source.write_text(harness);binary=tmp_path/"trace_test"
    vendor=ROOT/"tests/vendor/cjson"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-error=deprecated-declarations","-pthread",
        f"-DPET_P4_SPARE_OPEN_TRACE={int(spare_trace)}",
        "-fsanitize=address,undefined","-I",str(tmp_path),"-I",str(ROOT/"main"),"-I",str(vendor),
        str(source),str(vendor/"cJSON.c"),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True,timeout=20)
