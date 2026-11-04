"""Actual A/B catalog writer: every JSON allocation and filesystem failure."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_catalog_oom_never_writes_partial_json_and_io_keeps_active_generation(tmp_path):
    _catalog_writer_case(tmp_path,False)

def test_private_acceptance_catalog_oom_io_and_capacity_bound(tmp_path):
    _catalog_writer_case(tmp_path,True)

def _catalog_writer_case(tmp_path,acceptance):
    code = (ROOT / 'main/pet_p4_miniapp.c').read_text()
    atomic = function(code, 'write_catalog_atomic')
    writer = code[code.index('static bool write_catalog_snapshot_at('):code.index('/* Boot-only/non-yielding callers')]
    source = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_catalog_profile.h"
#define MINIAPP_CATALOG_VERSION 3
#define MINIAPP_CATALOG_JSON_MAX 16384
#define PET_WIDGET_TRACE_SCOPE(name,kind) int name __attribute__((unused))=0
#define pet_widget_trace_scope_end(name) ((void)(name))
#define vTaskDelay(ticks) ((void)(ticks))
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {char widget_id[48],title[64];uint8_t slot,package_generation;uint32_t widget_checksum,buttons_checksum,sprites_checksum;} miniapp_catalog_item_t;
typedef struct {uint8_t generation;uint32_t sequence;} miniapp_catalog_commit_t;
static char g_storage_failure[128];
static unsigned allocations,fail_allocation,live,writes,renames;
static int io_fault;
static bool work_current=true,g_catalog_recovery_blocked;
static bool storage_work_current(void){return work_current;}
static void *allocate(size_t n){allocations++;if(allocations==fail_allocation)return NULL;void *p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p){assert(live);live--;free(p);}}
static int catalog_find(const miniapp_catalog_item_t *items,size_t count,const char *id){for(size_t i=0;i<count;i++)if(!strcmp(items[i].widget_id,id))return (int)i;return -1;}
static void catalog_snapshot_path(uint8_t generation,bool tmp,char *out,size_t n){snprintf(out,n,"catalog-%u.%s",generation,tmp?"tmp":"json");}
static FILE *fault_open(const char *p,const char *mode){writes++;if(io_fault==1){errno=EACCES;return NULL;}return fopen(p,mode);}
static size_t fault_write(const void *data,size_t size,size_t n,FILE *f){if(io_fault==2){errno=ENOSPC;return 0;}return fwrite(data,size,n,f);}
static int fault_flush(FILE *f){int rc=fflush(f);if(io_fault==3){errno=EIO;return EOF;}return rc;}
static int fault_close(FILE *f){int rc=fclose(f);if(io_fault==7)work_current=false;if(io_fault==4){errno=EIO;return EOF;}return rc;}
static int fault_remove(const char *p){if(io_fault==5&&strstr(p,".json")){errno=EACCES;return -1;}return remove(p);}
static int fault_rename(const char *from,const char *to){if(io_fault==6){errno=EIO;return -1;}renames++;int rc=rename(from,to);if(io_fault==8)work_current=false;return rc;}
#define fopen fault_open
#define fwrite fault_write
#define fflush fault_flush
#define fclose fault_close
#define remove fault_remove
#define rename fault_rename
''' + atomic + r'''
#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef remove
#undef rename
''' + writer + r'''
static void put(const char *path,const char *text){FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void assert_text(const char *path,const char *text){FILE *f=fopen(path,"rb");assert(f);char buf[128]={0};fread(buf,1,sizeof(buf)-1,f);fclose(f);assert(!strcmp(buf,text));}
int main(void){
 cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);
 miniapp_catalog_item_t items[2]={{.widget_id="one",.title="One",.slot=3,.package_generation=1,.widget_checksum=11,.buttons_checksum=12,.sprites_checksum=13},
   {.widget_id="two",.title="Two",.slot=7,.package_generation=0,.widget_checksum=21,.buttons_checksum=22,.sprites_checksum=23}};
 miniapp_catalog_commit_t result={0};
 put("catalog-0.json","active-original");put("catalog-1.json","spare-original");
 assert(write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
 assert(result.sequence==51&&result.generation==1&&live==0&&renames==1);
 unsigned total=allocations;
 for(unsigned fail=1;fail<=total;fail++){
   put("catalog-1.json","spare-original");allocations=0;fail_allocation=fail;writes=0;
   result=(miniapp_catalog_commit_t){.generation=99,.sequence=999};
   assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
   assert(live==0&&writes==0&&result.generation==99&&result.sequence==999);
   assert_text("catalog-0.json","active-original");assert_text("catalog-1.json","spare-original");
 }
 fail_allocation=0;
 for(io_fault=1;io_fault<=6;io_fault++){
   g_catalog_recovery_blocked=false; /* independent boot fixture */
   put("catalog-1.json","spare-original");result=(miniapp_catalog_commit_t){99,999};
   assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
   assert(live==0&&result.generation==99&&result.sequence==999);
   assert_text("catalog-0.json","active-original");
   assert(g_catalog_recovery_blocked==(io_fault>=5));
   if(io_fault!=6)assert_text("catalog-1.json","spare-original");
 }
 // Revoke before replacing the destination: stop with the old generation intact.
 g_catalog_recovery_blocked=false;io_fault=7;put("catalog-1.json","spare-original");result=(miniapp_catalog_commit_t){99,999};
 assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));assert(result.generation==99);
 assert_text("catalog-1.json","spare-original");assert_text("catalog-0.json","active-original");
 // Revoke after rename: new bytes may be durable, but completion is UNKNOWN.
 assert(!g_catalog_recovery_blocked);
 work_current=true;io_fault=8;assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
 assert(g_catalog_recovery_blocked&&result.generation==99);
 unsigned prior_writes=writes;assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result)&&writes==prior_writes);
 g_catalog_recovery_blocked=false; /* Next independent boot fixture. */
 assert(result.generation==99&&result.sequence==999&&!work_current);
 work_current=true;io_fault=0;assert(write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
 FILE *f=fopen("catalog-1.json","rb");assert(f);char json[2048]={0};fread(json,1,sizeof(json)-1,f);fclose(f);
 cJSON *root=cJSON_Parse(json);assert(root);cJSON *array=cJSON_GetObjectItemCaseSensitive(root,"items");
 assert(cJSON_GetArraySize(array)==2&&cJSON_GetObjectItemCaseSensitive(root,"sequence")->valueint==51);
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root,"selectedWidgetId")->valuestring,"two"));
 assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root,"seedPending")));
 for(int i=0;i<2;i++)assert(cJSON_GetArraySize(cJSON_GetArrayItem(array,i))==7);
 cJSON_Delete(root);assert(!live);
 miniapp_catalog_item_t full[PET_P4_MINIAPP_CATALOG_MAX+1]={0};
 for(unsigned i=0;i<PET_P4_MINIAPP_CATALOG_MAX;i++){
   snprintf(full[i].widget_id,sizeof(full[i].widget_id),"item-%u",i);
   snprintf(full[i].title,sizeof(full[i].title),"Preserved item %u",i);full[i].slot=(uint8_t)i;
 }
 assert(write_catalog_snapshot_at(full,PET_P4_MINIAPP_CATALOG_MAX,"item-0","item-0",true,0,51,&result));
 f=fopen("catalog-1.json","rb");assert(f);char full_json[16384]={0};fread(full_json,1,sizeof(full_json)-1,f);fclose(f);
 root=cJSON_Parse(full_json);assert(root&&cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"items"))==PET_P4_MINIAPP_CATALOG_MAX);
 assert(cJSON_GetObjectItemCaseSensitive(root,"version")->valueint==3);
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root,"selectedWidgetId")->valuestring,"item-0"));
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"seedPending")));cJSON_Delete(root);assert(!live);
 unsigned before_writes=writes;result=(miniapp_catalog_commit_t){99,999};
 assert(!write_catalog_snapshot_at(full,PET_P4_MINIAPP_CATALOG_MAX+1,"item-0","item-0",true,0,51,&result));
 assert(writes==before_writes&&result.generation==99&&result.sequence==999);return 0;
}
'''
    src = tmp_path / 'catalog.c'
    src.write_text(source)
    exe = tmp_path / 'catalog'
    vendor = ROOT / 'tests/vendor/cjson'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-error=deprecated-declarations', '-fsanitize=address,undefined',
                    *(['-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
                       '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1',
                       '-DPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1'] if acceptance else []),
                    '-I', str(ROOT/'main'), '-I', str(vendor), str(src), str(vendor / 'cJSON.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=tmp_path, check=True)
