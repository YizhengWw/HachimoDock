"""Integrate actual writer, install/delete, sealed loader and epoch selector.
Only SDK scheduling/package IO/materialization are adapters; catalog filesystem
writes are real private files with after-success errno and authority faults.
"""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function
from petui_storage_mutation_test import _mutation_source
ROOT=Path(__file__).resolve().parents[1]

@pytest.fixture(scope='module',params=[0,1],ids=['atomic-rename','sealed-inplace'])
def binary(tmp_path_factory,request):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    source=_mutation_source(False)
    source=source.replace('int name=0','int name __attribute__((unused))=0')
    source=source.replace('assert(!held&&commits);','assert(!held&&g_catalog_sequence==21&&!g_catalog_recovery_blocked);')
    source=source.replace(function(source,'read_json_file'),'')
    # The same actual install/delete closure, replacing only its writer mock.
    stub=function(source,'write_catalog_snapshot_at')
    prefix=r"""
#define PET_P4_COMMIT_LEAN_IO LEAN_VALUE
#define MINIAPP_CATALOG_VERSION 3
#define MINIAPP_CATALOG_GENERATION_COUNT 2
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_CATALOG_JSON_MAX 16384
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include "pet_p4_file_read.h"
static int catalog_fault;
static unsigned writes_seen,removes_seen,renames_seen;
static void catalog_delay(unsigned n){(void)n;if(catalog_fault==6)work_current=false;}
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay catalog_delay
static FILE *cat_open(const char*p,const char*m){assert(!held);writes_seen++;if(catalog_fault==1){errno=EACCES;return NULL;}return fopen(p,m);}
static size_t cat_write(const void*p,size_t n,size_t z,FILE*f){assert(!held);if(catalog_fault==2)return fwrite(p,n,z/2,f);return fwrite(p,n,z,f);}
static int cat_flush(FILE*f){int rc=fflush(f);if(catalog_fault==3){errno=EIO;return EOF;}return rc;}
static int cat_close(FILE*f){int rc=fclose(f);if(catalog_fault==4){errno=EIO;return EOF;}return rc;}
static int cat_truncate(int fd,off_t len){int rc=ftruncate(fd,len);if(catalog_fault==5){errno=EIO;return -1;}return rc;}
static int cat_remove(const char*p){assert(!held);removes_seen++;int rc=remove(p);if(catalog_fault==5){errno=EIO;return -1;}return rc;}
static int cat_rename(const char*a,const char*b){assert(!held);renames_seen++;int rc=rename(a,b);if(catalog_fault==7){errno=EIO;return -1;}return rc;}
static void catalog_snapshot_path(uint8_t generation,bool tmp,char*out,size_t n){snprintf(out,n,"catalog-%u.%s",generation,tmp?"tmp":"json");}
#define fopen cat_open
#define fwrite cat_write
#define fflush cat_flush
#define fclose cat_close
#define ftruncate cat_truncate
#define remove cat_remove
#define rename cat_rename
"""
    prefix=prefix.replace('LEAN_VALUE',str(request.param))
    names=['catalog_file_matches','write_catalog_in_place','catalog_hex_digit','catalog_check_digits','catalog_check_value','catalog_check_seal','catalog_check_verify'] if request.param else ['write_catalog_atomic']
    if request.param:prefix+='#define MINIAPP_CATALOG_CHECK_KEY "\\"check\\":\\""\n'
    prefix+='\n'.join(function(code,n) for n in names)
    prefix+='\n'+function(code,'write_catalog_snapshot_at')+r"""
#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef ftruncate
#undef remove
#undef rename
typedef struct {
 bool parsed,valid,needs_upgrade;uint8_t file_generation;uint32_t sequence;
 char active_widget_id[48],selected_widget_id[48];bool seed_pending;
 miniapp_catalog_item_t items[PET_P4_MINIAPP_CATALOG_MAX];size_t count;
} miniapp_catalog_snapshot_t;
static bool read_json_file(const char*p,char*out,size_t n){FILE*f=fopen(p,"rb");if(!f)return false;size_t got=fread(out,1,n-1,f);bool ok=got&&fgetc(f)==EOF&&!ferror(f);out[got]=0;if(fclose(f))ok=false;return ok;}
static bool validate_catalog_snapshot(miniapp_catalog_snapshot_t*s){s->valid=true;return true;} /* Package materialization adapter only. */
"""
    prefix+='\n'.join(function(code,n) for n in ['parse_checksum_hex','load_catalog_snapshot','catalog_latest_snapshot'])
    source=source.replace(stub,prefix,1)
    main=function(source,'main')
    case=r"""
static bool fail_json;
static void*json_allocate(size_t n){return fail_json?NULL:malloc(n);}
static void json_release(void*p){free(p);}
static size_t bytes(const char*p,unsigned char*out,size_t n){FILE*f=fopen(p,"rb");if(!f)return 0;size_t z=fread(out,1,n,f);assert(!ferror(f));fclose(f);return z;}
int main(int argc,char **argv){
 assert(argc==3);bool deleting=atoi(argv[1])!=0;int fault=atoi(argv[2]);
 reset();assert(pet_storage_lane_configure(&g_storage_lane,unlock,lock,owner,NULL));
 held=false;miniapp_catalog_commit_t c={0};
 assert(write_catalog_snapshot_at(g_catalog,2,"garden","garden",false,0,18,&c)); /* spare19 */
 assert(write_catalog_snapshot_at(g_catalog,2,"garden","garden",false,1,19,&c)); /* active20 */
 held=true;
 cJSON*p=cJSON_Parse("{\"transferId\":\"transfer\",\"widgetId\":\"garden\"}");assert(p);
 unsigned char old[MINIAPP_CATALOG_JSON_MAX],after[MINIAPP_CATALOG_JSON_MAX];
 size_t old_n=bytes("catalog-0.json",old,sizeof(old));
 catalog_fault=fault==8?0:fault;
 if(fault==8){fail_json=true;cJSON_Hooks h={json_allocate,json_release};cJSON_InitHooks(&h);}
 writes_seen=removes_seen=renames_seen=0;
 assert(pet_storage_lane_enter(&g_storage_lane));char error[200]={0};
 bool ok=deleting?remove_component("garden",error,sizeof(error),true):pet_p4_miniapp_install_commit(p,error,sizeof(error));
 assert(held&&!g_storage_lane.occupied&&pet_storage_lane_leave(&g_storage_lane));
 fail_json=false;cJSON_InitHooks(NULL);
 bool uncertain=
#if PET_P4_COMMIT_LEAN_IO
 fault>=2&&fault<=6;
#else
 fault==5||fault==7;
#endif
 bool completed=fault==0
#if PET_P4_COMMIT_LEAN_IO
 ||fault==7
#endif
 ;
 assert(ok==completed&&g_catalog_recovery_blocked==uncertain);
 if(uncertain){
  assert(strstr(error,"unknown")&&strstr(error,"do not replay"));
  assert(g_catalog_sequence==20&&g_catalog_count==2&&cleanups==0);
  if(!deleting)assert(g_staging.active&&!strcmp(g_staging.transfer_id,"transfer"));
  unsigned observed=writes_seen,removed=removes_seen,renamed=renames_seen;
  assert(pet_storage_lane_enter(&g_storage_lane));
  assert(!pet_p4_miniapp_install_commit(p,error,sizeof(error)));
  assert(pet_storage_lane_leave(&g_storage_lane));
  assert(pet_storage_lane_enter(&g_storage_lane));
  assert(!remove_component("book",error,sizeof(error),true));
  assert(pet_storage_lane_leave(&g_storage_lane));
  assert(writes_seen==observed&&removes_seen==removed&&renames_seen==renamed);
 }else if(!completed){
  assert(g_catalog_sequence==20&&g_catalog_count==2&&cleanups==0);
  assert(!strstr(error,"unknown"));
 }else assert(g_catalog_sequence==21);
 assert(bytes("catalog-0.json",after,sizeof(after))==old_n&&!memcmp(old,after,old_n));
#if !PET_P4_COMMIT_LEAN_IO
 if(fault>=2&&fault<=6){FILE*proof=fopen("catalog-1.tmp","rb");assert(proof);assert(!fclose(proof));}
#endif
 /* Simulated cold metadata read: use the actual sealed loader and epoch
  * selector, NOT the failed API result/RAM catalog to infer durable outcome. */
 work_current=true;catalog_fault=0;
 miniapp_catalog_snapshot_t a={0},b={0};uint32_t sequence=0;
 (void)load_catalog_snapshot(0,&a);(void)load_catalog_snapshot(1,&b);
 miniapp_catalog_snapshot_t *picked=catalog_latest_snapshot(&a,&b,&sequence);
 assert(picked&&sequence>=20&&sequence<=21);
 /* Fault5 now fails the initial truncate0: no new JSON was written.
  * Still unknown/quarantined, but reboot must not invent the new epoch. */
 bool definitely_new=completed
#if PET_P4_COMMIT_LEAN_IO
 ||(fault>=3&&fault<=6&&fault!=5)
#else
 ||fault==7
#endif
 ;
 if(definitely_new)assert(sequence==21);
 if(!uncertain&&!completed)assert(sequence==20);
 if(sequence==21){
  assert(picked->count==(deleting?1u:2u));
  assert(catalog_find(picked->items,picked->count,"garden")==(deleting?-1:0));
 }
 cJSON_Delete(p);return 0;
}
"""
    source=source.replace(main,case,1)
    tmp=tmp_path_factory.mktemp('catalog-outcome');src=tmp/'case.c';src.write_text(source)
    exe=tmp/'case';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['/usr/bin/cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-error=deprecated-declarations',
      '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'main'),'-I',str(vendor),
      str(src),str(vendor/'cJSON.c'),str(ROOT/'main/pet_p4_storage_lane.c'),'-o',str(exe)],check=True,timeout=60)
    return exe
@pytest.mark.parametrize('deleting',[0,1],ids=['install','delete'])
@pytest.mark.parametrize('fault',range(9),ids=['success','open','short-write','flush','close','truncate-or-remove','revoke-after-close','rename-after-success','json-oom'])
def test_real_writer_handler_quarantine_and_reboot_epoch(binary,deleting,fault):
    subprocess.run([str(binary),str(deleting),str(fault)],cwd=binary.parent,check=True,timeout=15)
