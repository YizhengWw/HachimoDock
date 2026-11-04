"""Actual inactive-generation write: partial IO never changes the live pair.

Private temporary files only. No device, SDK mutation, audio or real components.
The unchanged catalog writer/readback/publication tests remain required too.
"""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def _spare_writer_case(tmp_path,recreate=False):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    helper = function(code, 'write_spare_package_json')
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pet_p4_storage_lane.h"
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_WIDGET_JSON_MAX 8192
#define MINIAPP_BUTTONS_JSON_MAX 2048
#define PET_WIDGET_TRACE_SCOPE(name,stage) int name=0
#define pet_widget_trace_scope_end(token) ((void)(token))
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ticks) ((void)(ticks))
typedef struct {unsigned slot,package_generation;} item_t;
static item_t g_catalog[32]={{3,0},{7,1}};
static size_t g_catalog_count=2;
static pet_storage_lane_t g_storage_lane;
static pet_storage_scope_t scope;
static char g_storage_failure[128];
static bool current=true;
static uintptr_t task=1;
static unsigned opens,closes;
static int fault;
#if PET_P4_SPARE_RECREATE
static unsigned removes;
static int remove_file(const char *p){removes++;if(fault==7){errno=EACCES;return -1;}int rc=remove(p);if(fault==8)current=false;return rc;}
#endif
static bool storage_work_current(void){return current;}
static uintptr_t owner(void *context){(void)context;return task;}
static void catalog_slot_path(size_t slot,uint8_t gen,bool buttons,bool tmp,char *out,size_t n){
 assert(!tmp);snprintf(out,n,"slot-%zu-%u-%u",slot,gen,buttons);
}
static FILE *open_file(const char *path,const char *mode){
 assert(!strcmp(mode,"wb"));opens++;if(fault==1){errno=EACCES;return NULL;}return fopen(path,mode);
}
static size_t write_file(const void *data,size_t size,size_t count,FILE *f){
 assert(size==1);
 if(fault==2){size_t written=fwrite(data,1,count/2,f);errno=ENOSPC;return written;}
 size_t written=fwrite(data,size,count,f);if(fault==5)current=false;return written;
}
static int flush_file(FILE *f){int rc=fflush(f);if(fault==3){errno=EIO;return EOF;}return rc;}
static int close_file(FILE *f){closes++;int rc=fclose(f);if(fault==6)current=false;if(fault==4){errno=EIO;return EOF;}return rc;}
#define fopen open_file
#define fwrite write_file
#define fflush flush_file
#define fclose close_file
#if PET_P4_SPARE_RECREATE
#define remove remove_file
#endif
''' + helper + r'''
#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef remove
static void put(const char *path,const char *text){FILE*f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void verify(const char *path,const char *text){FILE*f=fopen(path,"rb");assert(f);char out[80]={0};fread(out,1,79,f);assert(!fclose(f));assert(!strcmp(out,text));}
int main(void){
 scope=(pet_storage_scope_t){.owner=1,.active=true,.unlocked=true};
 g_storage_lane=(pet_storage_lane_t){.owner=owner,.scope_owner=&scope,.occupied=true};
 put("slot-3-0-0","live-widget");put("slot-3-0-1","live-buttons");
 put("slot-7-1-0","other-live");
 item_t original[32];memcpy(original,g_catalog,sizeof(original));
 /* Neither an explicit live slot nor another component's current slot is spare. */
 assert(!write_spare_package_json(3,0,false,"bad",3));
 assert(!write_spare_package_json(3,0,true,"bad",3));
 assert(!write_spare_package_json(7,1,false,"bad",3));
 assert(!write_spare_package_json(32,0,false,"bad",3));
 assert(!write_spare_package_json(3,2,false,"bad",3));assert(!opens);
 assert(!write_spare_package_json(3,1,false,"bad",8192));
 assert(!write_spare_package_json(3,1,true,"bad",2048));
 g_catalog_count=33;assert(!write_spare_package_json(3,1,false,"bad",3));g_catalog_count=2;
 task=2;assert(!write_spare_package_json(3,1,false,"bad",3));task=1;
 scope.unlocked=false;assert(!write_spare_package_json(3,1,false,"bad",3));scope.unlocked=true;
 g_storage_lane.occupied=false;assert(!write_spare_package_json(3,1,false,"bad",3));g_storage_lane.occupied=true;
 assert(!opens);
#if PET_P4_SPARE_RECREATE
 assert(!removes);
#endif
 for(fault=1;fault<=6+2*PET_P4_SPARE_RECREATE;fault++){
   current=true;opens=closes=0;put("slot-3-1-0","old-spare");
   assert(!write_spare_package_json(3,1,false,"new-widget",10));
   if(fault<=6){assert(opens==1&&closes==(fault!=1));}
   else {assert(!opens&&!closes);}
   verify("slot-3-0-0","live-widget");verify("slot-3-0-1","live-buttons");verify("slot-7-1-0","other-live");
   assert(!memcmp(original,g_catalog,sizeof(original))); /* never publishes */
 }
 fault=0;current=true;
 assert(write_spare_package_json(3,1,false,"new-widget",10));verify("slot-3-1-0","new-widget");
 assert(write_spare_package_json(3,1,true,"",0));verify("slot-3-1-1","");
 /* A new, unreferenced physical slot and boot's synchronous trusted scope. */
 g_storage_lane.owner=NULL;scope.owner=0;scope.unlocked=false;
 assert(write_spare_package_json(10,0,false,"new-slot",8));verify("slot-10-0-0","new-slot");
 assert(!memcmp(original,g_catalog,sizeof(original)));
 return 0;
}
'''
    src, exe = tmp_path / 'spare.c', tmp_path / 'spare'
    src.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DPET_P4_SPARE_RECREATE={int(recreate)}',
                    '-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True)


def test_real_spare_writer_rejects_live_generation_and_survives_torn_writes(tmp_path):
    _spare_writer_case(tmp_path)


def test_actual_spare_recreation_never_removes_live_files_or_publishes_failed_data(tmp_path):
    _spare_writer_case(tmp_path,True)


def test_catalog_publication_and_legacy_paths_keep_atomic_protocol():
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    commit=code.split('bool pet_p4_miniapp_install_commit(',1)[1].split('static bool commit_builtin_package(',1)[0]
    assert commit.count('write_spare_package_json(')==2
    assert commit.index('write_spare_package_json(')<commit.index('load_catalog_runtime_item(')<commit.index('write_catalog_snapshot_at(')
    writer = ''.join(function(code, 'write_catalog_snapshot_at').split())
    atomic = ''.join(function(code, 'write_catalog_atomic').split())
    inplace = ''.join(function(code, 'write_catalog_in_place').split())
    assert '#ifPET_P4_COMMIT_LEAN_IO' in writer
    assert 'write_catalog_atomic(catalog_tmp_path,catalog_path,json,strlen(json),&may_have_changed)' in writer
    assert 'write_catalog_in_place(catalog_path,json,strlen(json),&may_have_changed)' in writer
    assert atomic.index('fclose(file)') < atomic.index('*may_have_changed=true;') < atomic.index('remove(path)') < atomic.index('rename(tmp_path,path)')
    assert 'remove(tmp_path)' not in atomic  # No rollback deletion of evidence.
    assert inplace.index('*may_have_changed=true;') < inplace.index('ftruncate(fileno(file),0)') < inplace.index('fseek(file,0,SEEK_SET)') < inplace.index('fwrite(data,1,len,file)')
    assert inplace.index('fclose(file)') < inplace.index('catalog_file_matches(path,data,len)')
    verifier = ''.join(function(code, 'catalog_file_matches').split())
    assert 'unsignedcharblock[64];' in verifier and 'memcmp(block,data+offset,count)' in verifier
    assert 'fgetc(file)==EOF' in verifier and 'if(fclose(file)!=0)ok=false;' in verifier
    # Actual SDK NOR/old-writer negative + phase fault execution is provided by
    # p4_catalog_zero_rewrite_test, not replaced by these routing assertions.
    quarantine = writer[writer.index('if(!ok&&may_have_changed)'):writer.index('cJSON_free(json)')]
    assert 'g_catalog_recovery_blocked=true;' in quarantine
    assert 'do not replay' in function(code, 'write_catalog_snapshot_at')
    # The real writer+install/delete fault and reboot-selection behavior is
    # locked by p4_catalog_outcome_test, including close/rename-after-success;
    # this guard only keeps both actual production backends connected.
    assert 'write_spare_package_json(' not in function(code,'pet_p4_miniapp_init')
