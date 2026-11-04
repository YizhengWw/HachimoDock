"""Actual opt-in native optimization paths; no device or user files."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
from p4_render_trace_test import function


def _exercise_exact_spare_reuse(tmp_path,target=False,update=False,unbuffered=False):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    checksum=function(text,'miniapp_checksum').replace('miniapp_checksum(', 'native_checksum(')
    code=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "pet_p4_storage_lane.h"
#include "pet_p4_storage_io_trace.h"
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_WIDGET_JSON_MAX 8192
#define MINIAPP_BUTTONS_JSON_MAX 2048
#define PET_WIDGET_TRACE_SCOPE(name,stage) int name=0;(void)name
#define pet_widget_trace_scope_end(token) ((void)(token))
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ticks) ((void)(ticks))
typedef struct{unsigned slot,package_generation;uint32_t widget_checksum,buttons_checksum;uint8_t resource_state;}item_t;
static item_t g_catalog[32]={{.slot=3,.package_generation=0,.resource_state=1}};
static size_t g_catalog_count=1;
static uint8_t g_spare_json_seen[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT];
#if PET_P4_SPARE_TARGET_FILTER
static uint32_t g_spare_json_crc[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT][2];
static uint16_t g_spare_json_length[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT][2];
#endif
static pet_storage_lane_t g_storage_lane;
static pet_storage_scope_t scope;
static char g_storage_failure[128];
static bool current=true,audio,collision,last_read;
static uintptr_t task=1;
static unsigned reads,writes,physical_begins,physical_finishes;
enum{NONE,READ_OPEN,READ_DATA,READ_CLOSE,CANCEL_DATA,PHYSICAL_BEGIN,PHYSICAL_FINISH,CANCEL_WRITE_CLOSE,
 UPDATE_OPEN,WRITE_DATA,WRITE_FLUSH,WRITE_TRUNCATE,WRITE_CLOSE,CANCEL_WRITE_DATA,CANCEL_WRITE_FLUSH,CANCEL_WRITE_TRUNCATE,STREAM_POLICY};
static int fault;
static bool storage_work_current(void){return current;}
static bool unchanged_storage_current(void){return current&&!audio;}
static uintptr_t owner(void*context){(void)context;return task;}
static void catalog_slot_path(size_t slot,uint8_t gen,bool buttons,bool tmp,char*out,size_t n){assert(!tmp);snprintf(out,n,"slot-%zu-%u-%u",slot,gen,buttons);}
bool pet_storage_physical_begin(pet_storage_physical_scope_t*p){
 physical_begins++;assert(!p->self&&!p->task);if(fault==PHYSICAL_BEGIN)return false;p->self=p;p->task=(void*)1;return true;
}
bool pet_storage_physical_finish(pet_storage_physical_scope_t*p){
 physical_finishes++;assert(p->self==p&&p->task==(void*)1);*p=(pet_storage_physical_scope_t){0};return fault!=PHYSICAL_FINISH;
}
static FILE*open_file(const char*p,const char*mode){
 last_read=!strcmp(mode,"rb");assert(last_read||!strcmp(mode,"wb")||!strcmp(mode,"r+b"));
 if(!strcmp(mode,"r+b")&&fault==UPDATE_OPEN){errno=EIO;return NULL;}
 if(last_read){reads++;if(fault==READ_OPEN){errno=EIO;return NULL;}}else writes++;
 return fopen(p,mode);
}
static size_t read_file(void*p,size_t size,size_t count,FILE*f){
 if(fault==READ_DATA){errno=EIO;return 0;}size_t n=fread(p,size,count,f);if(fault==CANCEL_DATA)current=false;return n;
}
static int close_file(FILE*f){int result=fclose(f);if(!last_read&&fault==CANCEL_WRITE_CLOSE)current=false;if((last_read&&fault==READ_CLOSE)||(!last_read&&fault==WRITE_CLOSE)){errno=EIO;return EOF;}return result;}
#if PET_P4_SPARE_UPDATE
static unsigned truncates,write_calls;
static size_t write_file(const void*p,size_t size,size_t count,FILE*f){
 write_calls++;if(fault==WRITE_DATA){errno=EIO;return 0;}size_t n=fwrite(p,size,count,f);if(fault==CANCEL_WRITE_DATA)current=false;return n;
}
static int flush_file(FILE*f){
 if(fault==WRITE_FLUSH){errno=EIO;return EOF;}int result=fflush(f);if(fault==CANCEL_WRITE_FLUSH)current=false;return result;
}
static int truncate_file(int fd,off_t length){
 truncates++;if(fault==WRITE_TRUNCATE){errno=EIO;return -1;}
 int result=ftruncate(fd,length);if(fault==CANCEL_WRITE_TRUNCATE)current=false;return result;
}
#define fwrite write_file
#define fflush flush_file
#define ftruncate truncate_file
#endif
#if PET_P4_SPARE_UNBUFFERED
static unsigned policy_calls;
static int stream_policy(FILE *f,char *buffer,int mode,size_t size){
 policy_calls++;assert(!buffer&&mode==_IONBF&&!size);
 if(fault==STREAM_POLICY){errno=EINVAL;return -1;}
 return setvbuf(f,buffer,mode,size);
}
#define setvbuf stream_policy
#endif
#define fopen open_file
#define fread read_file
#define fclose close_file
#include "pet_p4_file_exact.h"
'''+checksum+r'''
static uint32_t miniapp_checksum(const char*p,size_t n){return collision?11:native_checksum(p,n);}
'''+function(text,'write_spare_package_json')+r'''
#undef fopen
#undef fread
#undef fclose
#undef fwrite
#undef fflush
#undef ftruncate
#undef setvbuf
static void put(const char*p,const char*data){FILE*f=fopen(p,"wb");assert(f);assert(fputs(data,f)>=0);assert(!fclose(f));}
static void verify(const char*p,const char*expected){FILE*f=fopen(p,"rb");assert(f);char data[100]={0};fread(data,1,99,f);assert(!fclose(f));assert(!strcmp(data,expected));}
static void reset(void){current=true;audio=collision=false;fault=NONE;reads=writes=physical_begins=physical_finishes=0;g_catalog[0].widget_checksum=native_checksum("same-widget",11);g_catalog[0].buttons_checksum=native_checksum("same-buttons",12);}
int main(void){
 scope=(pet_storage_scope_t){.owner=1,.active=true,.unlocked=true};
 g_storage_lane=(pet_storage_lane_t){.owner=owner,.scope_owner=&scope,.occupied=true};
 put("slot-3-0-0","live-widget");put("slot-3-0-1","live-buttons");reset();
 assert(!write_spare_package_json(3,0,false,"same-widget",11));assert(!reads&&!writes&&!physical_begins);
 task=2;assert(!write_spare_package_json(3,1,false,"same-widget",11));task=1;assert(!reads&&!writes);
 scope.unlocked=false;assert(!write_spare_package_json(3,1,false,"same-widget",11));scope.unlocked=true;
 scope.owner=2;assert(!write_spare_package_json(3,1,false,"same-widget",11));scope.owner=1;
 g_storage_lane.occupied=false;assert(!write_spare_package_json(3,1,false,"same-widget",11));g_storage_lane.occupied=true;
 g_storage_lane.scope_owner=NULL;assert(!write_spare_package_json(3,1,false,"same-widget",11));g_storage_lane.scope_owner=&scope;
 assert(!reads&&!writes&&!physical_begins);
 /* Unknown target: no read scan, even when the current checksum agrees. */
 put("slot-3-1-0","same-widget");assert(write_spare_package_json(3,1,false,"same-widget",11));
 assert(!reads&&writes==1&&!physical_begins&&g_spare_json_seen[3][1]==1);reset();
 put("slot-3-1-0","same-widget");assert(write_spare_package_json(3,1,false,"same-widget",11));
 assert(reads==1&&!writes&&physical_begins==1&&physical_finishes==1);
 reset();put("slot-3-1-1","same-buttons");assert(write_spare_package_json(3,1,true,"same-buttons",12));assert(!reads&&writes==1);reset();
 assert(write_spare_package_json(3,1,true,"same-buttons",12));assert(reads==1&&!writes);
 const char*wrong[]={"same-widge","same-widget-extra","wrong-data"};
 for(unsigned i=0;i<3;i++){reset();put("slot-3-1-0",wrong[i]);assert(write_spare_package_json(3,1,false,"same-widget",11));assert(reads==1&&writes==1);verify("slot-3-1-0","same-widget");}
 for(fault=READ_OPEN;fault<=READ_CLOSE;fault++){
  int selected=fault;reset();fault=selected;put("slot-3-1-0","same-widget");
  assert(write_spare_package_json(3,1,false,"same-widget",11));assert(reads==1&&writes==1&&physical_finishes==1);
 }
 for(int selected=CANCEL_DATA;selected<=PHYSICAL_FINISH;selected++){
  reset();fault=selected;put("slot-3-1-0","same-widget");assert(!write_spare_package_json(3,1,false,"same-widget",11));assert(!writes);
  assert(physical_finishes==(selected!=PHYSICAL_BEGIN));
 }
 reset();audio=true;assert(!write_spare_package_json(3,1,false,"same-widget",11));assert(!reads&&!writes&&!physical_begins);
 reset();g_catalog[0].widget_checksum++;assert(write_spare_package_json(3,1,false,"same-widget",11));
#if PET_P4_SPARE_TARGET_FILTER
 assert(reads==1&&!writes&&physical_begins==1);
 reset();g_spare_json_length[3][1][0]++;assert(write_spare_package_json(3,1,false,"same-widget",11));assert(!reads&&writes==1);
 reset();g_spare_json_crc[3][1][0]++;assert(write_spare_package_json(3,1,false,"same-widget",11));assert(!reads&&writes==1);
#else
 assert(!reads&&writes==1&&!physical_begins);
#endif
 reset();assert(write_spare_package_json(10,0,false,"new-slot",8));assert(!reads&&writes==1&&!physical_begins);
 reset();assert(!remove("slot-3-1-0"));assert(write_spare_package_json(3,1,false,"same-widget",11));assert(reads==1&&writes==(PET_P4_SPARE_UPDATE?2:1));
 reset();fault=CANCEL_WRITE_CLOSE;assert(!write_spare_package_json(11,0,false,"new-slot",8));assert(!reads&&writes==1&&!g_spare_json_seen[11][0]);
 reset();collision=true;g_catalog[0].widget_checksum=11;put("slot-3-1-0","wrong-data");
#if PET_P4_SPARE_TARGET_FILTER
 g_spare_json_crc[3][1][0]=11;
#endif
 assert(write_spare_package_json(3,1,false,"same-widget",11));assert(reads==1&&writes==1);verify("slot-3-1-0","same-widget");
#if PET_P4_SPARE_UPDATE
 /* Equal/growing/shrinking writes must end with exact EOF, including a stale
  * positive hint for a physically different or missing unpublished target. */
 const char *sizes[]={"equal-bytes","x","a-much-longer-widget-version",""};
 for(unsigned i=0;i<4;i++){
  reset();unsigned before=truncates;assert(write_spare_package_json(3,1,false,sizes[i],strlen(sizes[i])));
  assert(writes==1&&truncates==before+2);verify("slot-3-1-0",sizes[i]);
 }
 reset();assert(!remove("slot-3-1-0"));unsigned before=truncates;
 assert(write_spare_package_json(3,1,false,"after-missing",13));assert(writes==2&&truncates==before);verify("slot-3-1-0","after-missing");
 for(int selected=UPDATE_OPEN;selected<=CANCEL_WRITE_TRUNCATE;selected++){
  reset();put("slot-3-1-0","old-unpublished-tail");fault=selected;before=truncates;unsigned before_calls=write_calls;
  /* Deliberately different metadata: no exact-read path masks writer faults. */
  assert(!write_spare_package_json(3,1,false,"new",3));
  if(selected==UPDATE_OPEN)assert(!writes&&truncates==before); /* No EIO -> wb fallback. */
  else assert(writes==1);
  /* UPDATE now truncates zero BEFORE write. Failure/cancel at that first
   * truncate must not reach fwrite; post-length truncate remains separate. */
  if(selected==WRITE_DATA||selected==WRITE_FLUSH||selected==CANCEL_WRITE_FLUSH)assert(truncates==before+1);
  if(selected==WRITE_TRUNCATE||selected==CANCEL_WRITE_TRUNCATE)assert(truncates==before+1);
  if(selected==WRITE_TRUNCATE||selected==CANCEL_WRITE_TRUNCATE)assert(write_calls==before_calls);
  assert(g_catalog_count==1&&g_catalog[0].package_generation==0);
 }
#endif
#if PET_P4_SPARE_UNBUFFERED
 reset();put("slot-3-1-0","kept-spare");fault=STREAM_POLICY;
 unsigned before_writes=write_calls,before_truncates=truncates;
 assert(!write_spare_package_json(3,1,false,"policy-writer",13));
 assert(policy_calls&&write_calls==before_writes&&truncates==before_truncates);
 verify("slot-3-1-0","kept-spare");
#endif
 verify("slot-3-0-0","live-widget");verify("slot-3-0-1","live-buttons");
 assert(g_catalog_count==1&&g_catalog[0].slot==3&&g_catalog[0].package_generation==0&&g_catalog[0].resource_state==1);
 return 0;
}
'''
    src=tmp_path/'reuse.c';binary=tmp_path/'reuse';src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_SPARE_EXACT_REUSE=1','-DPET_P4_STORAGE_PRESERVE_UNCHANGED=1',
      f'-DPET_P4_SPARE_TARGET_FILTER={int(target)}',
      f'-DPET_P4_SPARE_UPDATE={int(update)}',
      f'-DPET_P4_SPARE_UNBUFFERED={int(unbuffered)}',
      '-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True)


def test_actual_exact_spare_reuse_keeps_legacy_failures_and_catalog(tmp_path):
    _exercise_exact_spare_reuse(tmp_path)


def test_actual_target_filter_reuses_exact_spare_not_other_live_version(tmp_path):
    _exercise_exact_spare_reuse(tmp_path,target=True)


def test_actual_unpublished_spare_update_keeps_exact_eof_and_writer_faults(tmp_path):
    _exercise_exact_spare_reuse(tmp_path,target=True,update=True)

def test_unbuffered_spare_keeps_all_original_fault_eof_and_cancel_vectors(tmp_path):
    _exercise_exact_spare_reuse(tmp_path,target=True,update=True,unbuffered=True)

def test_unbuffered_spare_cannot_escape_the_reviewed_update_parent(tmp_path):
    src=tmp_path/'unbuffered-guard.c';src.write_text('#include "pet_p4_storage_io_trace.h"\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_SPARE_UNBUFFERED=1',
      '-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Unbuffered spare writer requires the reviewed update parent' in result.stderr


def test_spare_update_requires_exact_target_parent(tmp_path):
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_storage_io_trace.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_SPARE_UPDATE=1',
      '-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Spare update requires the exact target-filter parent' in result.stderr


def test_target_filter_requires_the_exact_reuse_parent(tmp_path):
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_storage_io_trace.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_SPARE_TARGET_FILTER=1',
      '-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Target spare filter requires exact physical byte reuse' in result.stderr
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_span_target_parent':
            assert config[section]['extends']=='env:esp32_p4_glyph_rows_parent'
            assert 'PET_P4_SPARE_TARGET_FILTER=1' in flags
        else:assert 'PET_P4_SPARE_TARGET_FILTER=1' not in flags


def test_private_reuse_profile_does_not_grant_execution_or_change_transaction():
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_delta_parent':
            assert config[section]['extends']=='env:esp32_p4_raster_parent'
            assert 'PET_P4_SPARE_EXACT_REUSE=1' in flags and 'PET_P4_ALPHA_FAST=1' in flags
            assert 'PET_P4_SPARE_RECREATE' not in flags
            assert not any(token in flags for token in ('TASK_PRIORITY','F_CPU','PROGRAM_EXECUTION','PERIPHERAL'))
        else:assert 'PET_P4_SPARE_EXACT_REUSE=1' not in flags
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    helper=function(text,'write_spare_package_json')
    assert helper.index('package_generation==generation')<helper.index('pet_file_exact(')
    assert helper.index('pet_storage_physical_finish(')<helper.index('if(exact==PET_FILE_EXACT_MATCH)return true;')
    assert 'g_spare_json_seen[slot][generation]' in helper
    assert 'checksum==miniapp_checksum(data,length)' in helper
    commit=function(text,'pet_p4_miniapp_install_commit')
    assert commit.index('write_spare_package_json(')<commit.index('load_catalog_runtime_item(')<commit.index('write_catalog_snapshot_at(')
