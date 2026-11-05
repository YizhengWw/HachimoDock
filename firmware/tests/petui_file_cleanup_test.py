"""Production bounded directory plan + actual files and deterministic faults."""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def compile_run(tmp_path, source, name):
    path = tmp_path / f"{name}.c"
    path.write_text(source)
    binary = tmp_path / name
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"), str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=tmp_path, check=True, timeout=20)


def test_real_files_scan_failures_epoch_race_and_owner_loss(tmp_path):
    compile_run(tmp_path, r'''
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pet_p4_file_cleanup.h"
#define PET_WIDGET_TRACE_TEST 1
#include "pet_p4_widget_trace.h"
static unsigned opens,closes,reads,dir_reads,after_close_samples,removes,logs;
static unsigned scan_begins,scan_ends,trace_depth;
pet_widget_trace_scope_t pet_widget_trace_scope_begin(pet_widget_trace_stage_t stage){
 trace_depth++;if(stage==PET_WTRACE_FS_SCAN)scan_begins++;
 return(pet_widget_trace_scope_t){.stage=stage,.active=true};
}
void pet_widget_trace_scope_end(pet_widget_trace_scope_t*scope){
 if(scope->active){assert(trace_depth);trace_depth--;if(scope->stage==PET_WTRACE_FS_SCAN)scan_ends++;scope->active=false;}
}
static int fault;
static uint64_t epoch;
static bool owner=true,writing,dir_active;
static char log_line[384];
static void numeric_log(const char*tag,const char*format,...){
 (void)tag;va_list args;va_start(args,format);int len=vsnprintf(log_line,sizeof(log_line),format,args);va_end(args);
 assert(len>0&&(size_t)len<sizeof(log_line));assert(!strstr(log_line,"store")&&!strstr(log_line,"other"));
 assert(!dir_active&&!trace_depth&&scan_begins==opens&&scan_ends==opens);
 logs++;errno=ERANGE; /* output must not overwrite caller errno */
}
#define PET_FILE_CLEANUP_DIAGNOSTICS_TEST 1
#define ESP_LOGI(...) numeric_log(__VA_ARGS__)
static void put(const char *p){FILE *f=fopen(p,"wb");assert(f);assert(fwrite("x",1,1,f)==1);assert(!fclose(f));}
static bool exists(const char*p){return access(p,F_OK)==0;}
static bool owner_current(void*c){(void)c;return owner;}
static bool idle(void*c,uint64_t*out){
 (void)c;
 if(fault==17&&closes==1&&++after_close_samples==2)epoch+=2;
 *out=epoch;return !writing;
}
static DIR *fault_open(const char*p){
 opens++;dir_reads=0;
 if(fault==25&&opens==2){owner=false;errno=EIO;return NULL;}
 if(fault==1||(fault==19&&opens==2)){errno=EIO;return NULL;}
 DIR*d=opendir(p);dir_active=d!=NULL;return d;
}
static struct dirent *fault_read(DIR*d){
 reads++;dir_reads++;
 if(fault==10&&reads==2){put("store/late");epoch+=2;}
 if(fault==12&&reads==2)owner=false;
 if(fault==13&&reads==2)writing=true;
 if(fault==2&&dir_reads==2){errno=EIO;return NULL;}
 if(fault>=14&&fault<=25&&opens==1){
  if(dir_reads==1){static struct dirent stale;strcpy(stale.d_name,"late");return &stale;}
  errno=EIO;return NULL;
 }
 if(fault==18&&opens==2&&dir_reads==2)owner=false;
 if(fault==20&&opens==2&&dir_reads==2){epoch+=2;writing=true;}
 if(fault==22&&opens==2&&dir_reads==2){errno=EIO;return NULL;}
 if(fault==23&&opens==2){static struct dirent duplicate;strcpy(duplicate.d_name,"one");return &duplicate;}
 if(fault==24&&opens==2){static struct dirent bad;memset(&bad,'x',sizeof(bad));return &bad;}
 if(fault==4){static struct dirent long_name;memset(&long_name,'x',sizeof(long_name));return &long_name;}
 if(fault==5){static struct dirent endless;strcpy(endless.d_name,"unrelated");return &endless;}
 if(fault==6){static struct dirent duplicate;strcpy(duplicate.d_name,"one");return &duplicate;}
 return readdir(d);
}
static int fault_close(DIR*d){
 closes++;int result=closedir(d);dir_active=false;
 if(fault==7){put("store/late");epoch+=2;} // a writer/GC overlapped enumeration
 if(fault==8){put("store/late");owner=false;} // a new slot owner must be preserved
 if(fault==11)writing=true;
 if(fault==16&&closes==1)epoch+=2;
 if((fault==15&&closes==1)||(fault==21&&closes==2)){errno=0;return -1;}
 if(fault==3){errno=EIO;return -1;}
 return result;
}
static int fault_remove(const char*p){removes++;if(fault==9&&!strcmp(p,"store/one")){errno=EACCES;return -1;}return remove(p);}
#define opendir fault_open
#define readdir fault_read
#define closedir fault_close
#define remove fault_remove
#include "pet_p4_file_cleanup.c"
#undef opendir
#undef readdir
#undef closedir
#undef remove
static void reset(void){
 remove("store/one");remove("store/two");remove("store/late");
 opens=closes=reads=dir_reads=after_close_samples=removes=logs=scan_begins=scan_ends=0;
 assert(!trace_depth);fault=0;owner=true;writing=false;
}
int main(void){
 assert(!mkdir("store",0700));assert(!mkdir("other",0700));put("store/one-more");put("other/one");
 pet_file_cleanup_target_t targets[]={ {"store/one"},{"store/two"},{"store/late"} };
 pet_file_cleanup_guard_t guard={.owner_current=owner_current,.idle_epoch=idle};
 pet_file_cleanup_plan_t plan;
 reset();put("store/one");put("store/two");
 assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_SNAPSHOT&&plan.diagnostic.found==3);
 assert(plan.diagnostic.attempts==1&&plan.diagnostic.first_reason==PET_CLEANUP_SNAPSHOT);
 assert(plan.diagnostic.observer_before==1&&plan.diagnostic.observer_after==1&&!plan.diagnostic.activity);
 assert(strstr(log_line,"used=1 reason=0 n=3")&&plan.diagnostic.entries>=3);
 for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
 assert(opens==1&&closes==1&&removes==2&&!exists("store/one")&&!exists("store/two"));
 assert(exists("other/one")&&exists("store/one-more"));
 reset();assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&plan.snapshot_used);
 for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
 assert(removes==0);
 for(int failure=1;failure<=7;failure++){
  reset();put("store/one");put("store/two");fault=failure;
  assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
  const unsigned reasons[]={0,PET_CLEANUP_OPEN_FAILED,PET_CLEANUP_READ_FAILED,PET_CLEANUP_CLOSE_FAILED,
   PET_CLEANUP_NAME_UNTERMINATED,PET_CLEANUP_ENTRY_LIMIT,PET_CLEANUP_DUPLICATE_TARGET,PET_CLEANUP_EPOCH_AFTER};
  assert(logs==1&&plan.diagnostic.reason==reasons[failure]&&strstr(log_line,"used=0"));
  if(failure==1)assert(plan.diagnostic.open_errno==EIO&&errno==EIO);
  if(failure==2)assert(plan.diagnostic.read_errno==EIO);
  if(failure==3)assert(plan.diagnostic.close_errno==EIO);
  if(failure==7)assert(plan.diagnostic.activity&PET_CLEANUP_ACTIVITY_EPOCH_CHANGED);
  for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
  assert(removes==3&&!exists("store/one")&&!exists("store/two")&&!exists("store/late"));
  assert(opens==(failure==2?2U:1U)&&closes==(failure==1?0U:failure==2?2U:1U));
  if(failure==2)assert(plan.diagnostic.attempts==2&&plan.diagnostic.first_read_errno==EIO);
  if(failure==5)assert(reads==PET_FILE_CLEANUP_MAX_ENTRIES+1);
 }
 reset();put("store/one");fault=8;
 assert(!pet_file_cleanup_prepare(&plan,"store",targets,3,guard));
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OWNER_AFTER);
 for(unsigned i=0;i<3;i++)assert(!pet_file_cleanup_remove_at(&plan,i));
 assert(!removes&&exists("store/one")&&exists("store/late"));
 reset();put("store/one");assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard));owner=false;
 assert(!pet_file_cleanup_remove_at(&plan,0)&&exists("store/one")&&!removes);
 reset();put("store/one");put("store/two");fault=9;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard));bool ok=true;
 for(unsigned i=0;i<3;i++)ok=pet_file_cleanup_remove_at(&plan,i)&&ok;
 assert(!ok&&exists("store/one")&&!exists("store/two")&&removes==2);
 reset();writing=true;assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OBSERVER_BEFORE);
 assert(plan.diagnostic.observer_before==2&&!plan.diagnostic.observer_after);
 assert(plan.diagnostic.activity==PET_CLEANUP_ACTIVITY_UNAVAILABLE);
 assert(!opens);for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));assert(removes==3);
 reset();put("store/one");fault=10;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_EPOCH_DURING);
 assert(plan.diagnostic.epoch_after==plan.diagnostic.epoch_before+2);
 assert(reads==2&&closes==1); /* do not finish a scan after concurrent activity */
 for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
 assert(removes==3&&!exists("store/one")&&!exists("store/late"));
 reset();fault=11;assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OBSERVER_AFTER);
 assert(plan.diagnostic.observer_before==1&&plan.diagnostic.observer_after==2);
 reset();fault=12;assert(!pet_file_cleanup_prepare(&plan,"store",targets,3,guard));
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OWNER_DURING&&!plan.valid);
 reset();fault=13;assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OBSERVER_DURING);
 /* A new complete scan is independent; a fake first-scan 'late' entry must
  * not survive into its bitmap. There is never a removal before both scans. */
 reset();put("store/one");fault=14;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&plan.snapshot_used);
 assert(logs==1&&opens==2&&closes==2&&!removes&&plan.present==1);
 assert(plan.diagnostic.attempts==2&&plan.diagnostic.first_reason==PET_CLEANUP_READ_FAILED);
 assert(plan.diagnostic.first_read_errno==EIO&&plan.diagnostic.first_entries==1);
 assert(plan.diagnostic.first_flags&PET_CLEANUP_DIAG_CLOSED_OK);
 assert(plan.diagnostic.reason==PET_CLEANUP_SNAPSHOT&&!plan.diagnostic.read_errno);
 for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
 assert(removes==1);
 /* Error with a failed close (even errno=0), or activity after the first
  * scan, cannot authorize a second open. No new baseline is accepted. */
 for(int f=15;f<=17;f++){
  reset();put("store/one");fault=f;
  assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
  assert(logs==1&&opens==1&&closes==1&&plan.diagnostic.attempts==1);
  assert(plan.diagnostic.first_reason==PET_CLEANUP_READ_FAILED&&plan.diagnostic.first_read_errno==EIO);
  if(f==15)assert(!plan.diagnostic.close_errno&&!(plan.diagnostic.flags&PET_CLEANUP_DIAG_CLOSED_OK));
  else assert(plan.diagnostic.activity&PET_CLEANUP_ACTIVITY_EPOCH_CHANGED);
  for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
  assert(removes==3);
 }
 reset();put("store/one");fault=18;
 assert(!pet_file_cleanup_prepare(&plan,"store",targets,3,guard));
 assert(logs==1&&opens==2&&closes==2&&!plan.valid&&plan.diagnostic.attempts==2);
 assert(plan.diagnostic.reason==PET_CLEANUP_OWNER_DURING&&plan.diagnostic.first_read_errno==EIO);
 for(unsigned i=0;i<3;i++)assert(!pet_file_cleanup_remove_at(&plan,i));
 assert(!removes&&exists("store/one"));
 for(int f=19;f<=24;f++){
  reset();put("store/one");fault=f;
  assert(pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.snapshot_used);
  assert(logs==1&&opens==2&&closes==(f==19?1U:2U)&&plan.diagnostic.attempts==2);
  assert(plan.diagnostic.first_reason==PET_CLEANUP_READ_FAILED&&plan.diagnostic.first_entries==1);
  for(unsigned i=0;i<3;i++)assert(pet_file_cleanup_remove_at(&plan,i));
  assert(removes==3);
 }
 reset();put("store/one");fault=25;
 assert(!pet_file_cleanup_prepare(&plan,"store",targets,3,guard)&&!plan.valid);
 assert(logs==1&&opens==2&&closes==1&&plan.diagnostic.reason==PET_CLEANUP_OWNER_AFTER);
 assert(plan.diagnostic.first_read_errno==EIO&&plan.diagnostic.open_errno==EIO);
 for(unsigned i=0;i<3;i++)assert(!pet_file_cleanup_remove_at(&plan,i));
 assert(!removes&&exists("store/one"));
 reset();pet_file_cleanup_guard_t no_observer=guard;no_observer.idle_epoch=NULL;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,3,no_observer)&&!plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OBSERVER_MISSING&&!opens);
 reset();assert(pet_file_cleanup_prepare(&plan,"store",targets,0,guard)&&!plan.snapshot_used);
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_EMPTY&&!opens);
 reset();owner=false;assert(!pet_file_cleanup_prepare(&plan,"store",targets,3,guard));
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_OWNER_BEFORE&&!opens);
 reset();pet_file_cleanup_target_t bad[]={{"other/one"},{"store/../one"}};
 assert(!pet_file_cleanup_prepare(&plan,"store",bad,1,guard));
 assert(logs==1&&plan.diagnostic.reason==PET_CLEANUP_INVALID_TARGET);
 assert(!pet_file_cleanup_prepare(&plan,"store",bad+1,1,guard));
 assert(!pet_file_cleanup_prepare(&plan,"store",targets,PET_FILE_CLEANUP_MAX_TARGETS+1,guard));
 assert(plan.diagnostic.reason==PET_CLEANUP_INVALID_ARGUMENT);
 memset(bad[0].path,'x',sizeof(bad[0].path));assert(!pet_file_cleanup_prepare(&plan,"store",bad,1,guard));
 bad[0]=targets[0];bad[1]=targets[0];assert(!pet_file_cleanup_prepare(&plan,"store",bad,2,guard));
 assert(!opens&&!removes&&exists("other/one"));
 reset();assert(!pet_file_cleanup_prepare(NULL,"store",targets,3,guard)&&logs==1);
 /* Diagnostic width is bounded even at numeric extremes, with no file data. */
 cleanup_report_t extreme={.value={.epoch_before=UINT64_MAX,.epoch_after=UINT64_MAX,
  .found=UINT32_MAX,.open_errno=INT32_MIN,.read_errno=INT32_MIN,.close_errno=INT32_MIN,
  .entries=UINT16_MAX,.reason=UINT8_MAX,.flags=UINT8_MAX,.activity=UINT8_MAX,
  .observer_before=UINT8_MAX,.observer_after=UINT8_MAX,.targets=UINT8_MAX,
  .first_read_errno=INT32_MIN,.first_entries=UINT16_MAX,.attempts=UINT8_MAX,.first_reason=UINT8_MAX,.first_flags=UINT8_MAX}};
 cleanup_report_end(&extreme);assert(strlen(log_line)<384);
 return 0;
}
''', "cleanup_faults")


@pytest.mark.parametrize("sprites", [4, 11])
def test_real_package_targets_and_unused_sprite_generations(tmp_path, sprites):
    source = (ROOT / "main/pet_p4_miniapp.c").read_text()
    names = ("catalog_slot_path", "catalog_sprite_path", "cleanup_owner_current", "cleanup_storage_idle", "cleanup_guard",
             "remove_file_if_present", "component_cleanup_targets", "persist_sprite_pack")
    extracted = "\n".join(function(source, name) for name in names).replace('"/spiffs', '"store')
    defines = "\n".join(line for line in source.splitlines() if line.startswith("#define MINIAPP_")
        and ("_PATH " in line or line.startswith("#define MINIAPP_DELETE_TARGETS")))
    defines = defines.replace('"/spiffs', '"store')
    defines += f"\n#define PET_P4_MINIAPP_SPRITE_MAX {sprites}\n"
    delete = function(source, "remove_component")
    batch = delete[delete.index("  pet_file_cleanup_target_t targets[MINIAPP_DELETE_TARGETS];"):delete.index("  if (!package_cleanup_ok) {")].replace('"/spiffs', '"store')
    batch = "static bool pet_app_store_forget_with_lease(const char *id,int lease){assert(id&&lease);abort();}\n#define g_storage_resource_lease 1\nstatic void set_error(char *e,size_t n,const char *s){(void)e;(void)n;(void)s;abort();}\nstatic bool cleanup_package(size_t removed_slot){char error[1];size_t error_size=sizeof(error);const char *widget_id=\"fixture\";bool preserve_save=true;\n" + batch + "return package_cleanup_ok;}\n"
    compile_run(tmp_path, f'#define PET_P4_MINIAPP_SPRITE_MAX {sprites}\n' + r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pet_p4_widget_trace.h"
#include "pet_p4_file_cleanup.c"
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_SPRITE_FILE_MAX 256
#define MINIAPP_SPRITE_FILE_HEADER_SIZE 8
typedef struct{uint8_t fps,frame_width,frame_height,frames;size_t data_offset,data_length;} sprite_t;
typedef sprite_t pet_p4_miniapp_sprite_t;
typedef struct{uint8_t count;size_t data_length;sprite_t items[PET_P4_MINIAPP_SPRITE_MAX];uint8_t pixels[128];} pet_p4_miniapp_sprite_pack_t;
static uint32_t g_catalog_sequence=4;
static char g_storage_failure[160];
static bool storage_work_current(void){return true;}
/* This file tests standalone cleanup/persistence, not the admitted spare-write
 * lane. Entering that unconfigured lane would be a harness error. */
static struct{bool occupied;}g_storage_lane;
static bool write_spare_sprite(size_t slot,uint8_t gen,uint8_t idx,const uint8_t*p,size_t n){(void)slot;(void)gen;(void)idx;(void)p;(void)n;abort();}
static bool remove_file_if_present(const char*);
static bool pet_fs_activity_idle_epoch(uint64_t*out){*out=0;return true;}
static void put(const char*p,const char*s){FILE*f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
static bool exists(const char*p){return access(p,F_OK)==0;}
static bool write_file_atomic(const char*tmp,const char*path,const char*data,size_t n){FILE*f=fopen(tmp,"wb");if(!f)return false;bool ok=fwrite(data,1,n,f)==n;ok=fclose(f)==0&&ok;if(!ok)return false;remove(path);return rename(tmp,path)==0;}
''' + defines + "\n" + extracted + "\n" + batch + r'''
int main(void){
 assert(!mkdir("store",0700));put("store/as-private-0","save");
 pet_file_cleanup_target_t targets[MINIAPP_DELETE_TARGETS],other[MINIAPP_DELETE_TARGETS];
 component_cleanup_targets(7,targets);component_cleanup_targets(8,other);
 assert(MINIAPP_DELETE_TARGETS==6+2*(2+PET_P4_MINIAPP_SPRITE_MAX));
 for(size_t i=0;i<MINIAPP_DELETE_TARGETS;i++)put(targets[i].path,"old");
 // Slots share the six legacy targets, but not their per-slot temporary paths.
 for(size_t i=2;i<MINIAPP_DELETE_TARGETS-4;i++)put(other[i].path,"new slot");
 char path[48];for(uint8_t gen=0;gen<2;gen++){
  catalog_slot_path(7,gen,false,false,path,sizeof(path));put(path,"widget");
  catalog_slot_path(7,gen,true,false,path,sizeof(path));put(path,"buttons");
  for(uint8_t i=0;i<PET_P4_MINIAPP_SPRITE_MAX;i++){catalog_sprite_path(7,gen,i,false,path,sizeof(path));put(path,"sprite");}
 }
 uint32_t seq=g_catalog_sequence;pet_file_cleanup_plan_t plan;
 assert(cleanup_package(7));
 for(size_t i=0;i<MINIAPP_DELETE_TARGETS;i++)assert(!exists(targets[i].path));
 for(size_t i=2;i<MINIAPP_DELETE_TARGETS-4;i++)assert(exists(other[i].path));
 for(uint8_t gen=0;gen<2;gen++){
  catalog_slot_path(7,gen,false,false,path,sizeof(path));assert(exists(path));
  catalog_slot_path(7,gen,true,false,path,sizeof(path));assert(exists(path));
  for(uint8_t i=0;i<PET_P4_MINIAPP_SPRITE_MAX;i++){catalog_sprite_path(7,gen,i,false,path,sizeof(path));assert(exists(path));}
 }
 assert(exists("store/as-private-0"));
 // Real persist function only removes unused images of the selected generation.
 for(uint8_t gen=0;gen<2;gen++)for(uint8_t i=0;i<PET_P4_MINIAPP_SPRITE_MAX;i++){catalog_sprite_path(7,gen,i,true,path,sizeof(path));put(path,"tmp");}
 pet_p4_miniapp_sprite_pack_t pack={.count=2,.data_length=2,.items={{.data_length=1},{.data_offset=1,.data_length=1}},.pixels={4,9}};
 assert(persist_sprite_pack(7,1,&pack));
 for(uint8_t i=0;i<PET_P4_MINIAPP_SPRITE_MAX;i++){
  catalog_sprite_path(7,0,i,false,path,sizeof(path));assert(exists(path));
  catalog_sprite_path(7,0,i,true,path,sizeof(path));assert(exists(path));
  catalog_sprite_path(7,1,i,false,path,sizeof(path));assert(exists(path)==(i<2));
  catalog_sprite_path(7,1,i,true,path,sizeof(path));assert(!exists(path));
 }
 pack.count=0;assert(persist_sprite_pack(7,0,&pack));
 for(uint8_t i=0;i<PET_P4_MINIAPP_SPRITE_MAX;i++)for(unsigned temp=0;temp<2;temp++){catalog_sprite_path(7,0,i,temp,path,sizeof(path));assert(!exists(path));}
 catalog_sprite_path(7,1,0,false,path,sizeof(path));assert(exists(path));assert(exists("store/as-private-0"));
 // A stale prepared owner cannot delete a newly reused slot's file.
 component_cleanup_targets(8,targets);seq=g_catalog_sequence;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,MINIAPP_DELETE_TARGETS<PET_FILE_CLEANUP_MAX_TARGETS?MINIAPP_DELETE_TARGETS:PET_FILE_CLEANUP_MAX_TARGETS,cleanup_guard(&seq)));
 g_catalog_sequence++;put(targets[2].path,"reused");assert(!pet_file_cleanup_remove_at(&plan,2)&&exists(targets[2].path));
 return 0;
}
''', "cleanup_packages")
    delete = function(source, "remove_component")
    assert delete.index("write_catalog_snapshot_at") < delete.index("PET_WTRACE_PUBLISH") < delete.index("pet_file_cleanup_prepare")
    assert delete.index("PET_STORAGE_DURABLE") < delete.index("g_catalog_sequence=commit.sequence")
    assert delete.index("PET_STORAGE_CLEANUP") < delete.index("pet_file_cleanup_prepare")
    assert delete.index("pet_file_cleanup_remove_at(&cleanup,0)") < delete.index("pet_app_store_forget") < delete.index("for(size_t target=1")
