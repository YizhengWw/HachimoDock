"""R131 lean commit IO: in-place self-checked catalog generations, sprite cleanup memo."""
from pathlib import Path
import configparser
import re
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def _catalog_source(code):
    key=re.search(r'#define MINIAPP_CATALOG_CHECK_KEY .*\n',code).group(0)
    writer=code[code.index('static bool write_catalog_snapshot_at('):code.index('/* Boot-only/non-yielding callers')]
    helpers=''.join(function(code,n)+'\n' for n in ('catalog_file_matches','write_catalog_in_place','catalog_hex_digit','catalog_check_digits','catalog_check_value','catalog_check_seal','catalog_check_verify'))
    return r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "cJSON.h"
#include "pet_p4_file_read.h"
#include "pet_p4_json_source.h"
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
static unsigned allocations,fail_allocation,live,opens,creates,truncates,zero_truncates,verify_reads;
static int io_fault;
static bool work_current=true,g_catalog_recovery_blocked;
static bool storage_work_current(void){return work_current;}
static void *allocate(size_t n){allocations++;if(allocations==fail_allocation)return NULL;void *p=malloc(n);if(p)live++;return p;}
static void release(void *p){if(p){assert(live);live--;free(p);}}
static int catalog_find(const miniapp_catalog_item_t *items,size_t count,const char *id){for(size_t i=0;i<count;i++)if(!strcmp(items[i].widget_id,id))return (int)i;return -1;}
static void catalog_snapshot_path(uint8_t generation,bool tmp,char *out,size_t n){snprintf(out,n,"catalog-%u.%s",generation,tmp?"tmp":"json");}
static FILE *fault_open(const char *p,const char *mode){opens++;if(!strcmp(mode,"wb"))creates++;if(io_fault==1){errno=EACCES;return NULL;}return fopen(p,mode);}
static size_t fault_write(const void *data,size_t size,size_t n,FILE *f){if(io_fault==2){errno=ENOSPC;return 0;}return fwrite(data,size,n,f);}
static int fault_flush(FILE *f){int rc=fflush(f);if(io_fault==3){errno=EIO;return EOF;}return rc;}
static int fault_truncate(int fd,off_t len){truncates++;if(!len)zero_truncates++;if(io_fault==5){errno=EIO;return -1;}if(io_fault==7)work_current=false;return ftruncate(fd,len);}
static int fault_close(FILE *f){int rc=fclose(f);if(io_fault==4){errno=EIO;return EOF;}return rc;}
static size_t fault_read(void *p,size_t size,size_t n,FILE *f){verify_reads++;assert(size==1&&n<=64);return fread(p,size,n,f);}
#define fread fault_read
#define fopen fault_open
#define fwrite fault_write
#define fflush fault_flush
#define fclose fault_close
#define ftruncate fault_truncate
'''+key+helpers+r'''
#undef fread
#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef ftruncate
'''+writer


MAIN=r'''
static void put(const char *path,const char *text){FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static size_t slurp(const char *path,char *buf,size_t n){FILE *f=fopen(path,"rb");if(!f)return 0;size_t len=fread(buf,1,n-1,f);fclose(f);buf[len]=0;return len;}
/* Actual source-text guard plus first-value self-check; catalog schema is
 * exercised by the existing actual-loader membership/outcome tests. */
static bool loader_accepts(const char *text){
 if(!pet_json_source_bounded_text(text))return false;
 const char *end=NULL;cJSON *root=cJSON_ParseWithOpts(text,&end,0);
 bool ok=root&&end&&end>text&&catalog_check_verify(text,(size_t)(end-text));
 if(ok)ok=cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root,"sequence"));
 cJSON_Delete(root);return ok;
}
static int sequence_of(const char *text){const char *end=NULL;cJSON *root=cJSON_ParseWithOpts(text,&end,0);int s=root?cJSON_GetObjectItemCaseSensitive(root,"sequence")->valueint:-1;cJSON_Delete(root);return s;}
int main(void){
 cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);
 miniapp_catalog_item_t items[2]={{.widget_id="one",.title="One \"quoted\" check\":\"deadbeef",.slot=3,.package_generation=1,.widget_checksum=11,.buttons_checksum=12,.sprites_checksum=13},
   {.widget_id="two",.title="Two",.slot=7,.package_generation=0,.widget_checksum=21,.buttons_checksum=22,.sprites_checksum=23}};
 miniapp_catalog_commit_t result={0};static char text[MINIAPP_CATALOG_JSON_MAX],old[MINIAPP_CATALOG_JSON_MAX],mixed[MINIAPP_CATALOG_JSON_MAX*2];
 remove("catalog-1.json");put("catalog-0.json","active-original");
 /* Absent inactive generation: one create (wb), never a tmp/remove/rename. */
 assert(write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
 assert(result.sequence==51&&result.generation==1&&live==0&&creates==1&&truncates==2&&zero_truncates==1&&verify_reads>0);
 size_t len=slurp("catalog-1.json",text,sizeof(text));assert(len>0&&loader_accepts(text)&&sequence_of(text)==51);
 assert(strstr(text,"\"check\":\"")&&!strstr(text,"\"check\":\"00000000\""));
 /* Existing longer inactive generation: in-place update truncates, content exact. */
 memset(old,'x',sizeof(old)-1);old[sizeof(old)-1]=0;put("catalog-1.json",old);creates=0;
 assert(write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result)&&creates==0&&result.sequence==51);
 size_t len2=slurp("catalog-1.json",text,sizeof(text));assert(len2==len&&loader_accepts(text));
 /* Legacy snapshot without a check stays accepted; a corrupted digit is rejected. */
 assert(loader_accepts("{\"version\":3,\"sequence\":9,\"activeWidgetId\":null,\"items\":[]}"));
 strcpy(mixed,text);char *d=strstr(mixed,"\"check\":\"")+9;d[0]=d[0]=='0'?'1':'0';assert(!loader_accepts(mixed));
 strcpy(mixed,text);d=strstr(mixed,"\"check\":\"");d[9]='g';assert(!loader_accepts(mixed));
 /* A check-valid prefix does NOT bypass the actual whole-source guard. */
 snprintf(mixed,sizeof(mixed),"%s%s",text,"]}garbage{\"sequence\":99,\"check\":\"00000000\"}");assert(!loader_accepts(mixed));
 /* Torn mixes of a new prefix and an old tail never pass as a valid snapshot. */
 miniapp_catalog_item_t many[PET_P4_MINIAPP_CATALOG_MAX]={0};
 for(unsigned i=0;i<PET_P4_MINIAPP_CATALOG_MAX;i++){snprintf(many[i].widget_id,48,"item-%u",i);snprintf(many[i].title,64,"Title %u",i);many[i].slot=(uint8_t)i;}
 assert(write_catalog_snapshot_at(many,PET_P4_MINIAPP_CATALOG_MAX,"item-0","item-1",false,1,51,&result)&&result.generation==0&&result.sequence==52);
 size_t long_len=slurp("catalog-0.json",old,sizeof(old));assert(long_len>len&&loader_accepts(old));
 unsigned rejected=0;
 /* Both snapshots share a prefix, so a cut inside it leaves the OLD file byte-identical: that is still a valid old snapshot. */
 for(size_t cut=8;cut<len;cut+=7){memcpy(mixed,text,cut);strcpy(mixed+cut,old+cut);if(!loader_accepts(mixed))rejected++;else assert(!memcmp(mixed,old,long_len)||!memcmp(mixed,text,len));}
 assert(rejected>0);
 for(size_t cut=8;cut<long_len;cut+=37){memcpy(mixed,old,cut);strcpy(mixed+cut,text+(cut<len?cut:len));assert(!loader_accepts(mixed)||!memcmp(mixed,old,long_len)||!memcmp(mixed,text,len));}
 /* Allocation failures write nothing and keep both generations byte-identical. */
 put("catalog-1.json","spare-original");put("catalog-0.json","active-original");allocations=0;
 assert(write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));unsigned total=allocations;
 for(unsigned fail=1;fail<=total;fail++){
  put("catalog-1.json","spare-original");allocations=0;fail_allocation=fail;opens=0;
  result=(miniapp_catalog_commit_t){.generation=99,.sequence=999};
  assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
  assert(live==0&&opens==0&&result.generation==99&&result.sequence==999);
  slurp("catalog-1.json",text,sizeof(text));assert(!strcmp(text,"spare-original"));slurp("catalog-0.json",text,sizeof(text));assert(!strcmp(text,"active-original"));
 }
 fail_allocation=0;
 /* IO faults: never published; the ACTIVE generation is untouched; the inactive
  * file is either unchanged, torn (rejected by the loader) or complete. */
 for(io_fault=1;io_fault<=5;io_fault++){
   g_catalog_recovery_blocked=false; /* independent boot fixture */
  put("catalog-1.json","{\"version\":3,\"sequence\":7,\"activeWidgetId\":null,\"items\":[]}");result=(miniapp_catalog_commit_t){99,999};
  assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result));
  assert(live==0&&result.generation==99&&result.sequence==999);
  slurp("catalog-0.json",text,sizeof(text));assert(!strcmp(text,"active-original"));
  slurp("catalog-1.json",text,sizeof(text));
  if(loader_accepts(text))assert(sequence_of(text)==7||sequence_of(text)==51);
 }
 /* Revoke during truncate: not reported as published. */
 g_catalog_recovery_blocked=false;io_fault=7;work_current=true;result=(miniapp_catalog_commit_t){99,999};
 assert(!write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result)&&result.generation==99);
 assert(g_catalog_recovery_blocked);g_catalog_recovery_blocked=false;work_current=true;io_fault=0;assert(write_catalog_snapshot_at(items,2,"one","two",false,0,50,&result)&&result.sequence==51);
 slurp("catalog-1.json",text,sizeof(text));assert(loader_accepts(text));
 cJSON *root=cJSON_Parse(text);cJSON *array=cJSON_GetObjectItemCaseSensitive(root,"items");
 assert(cJSON_GetArraySize(array)==2);for(int i=0;i<2;i++)assert(cJSON_GetArraySize(cJSON_GetArrayItem(array,i))==7);
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(array,0),"title")->valuestring,items[0].title));
 cJSON_Delete(root);assert(!live);
 result=(miniapp_catalog_commit_t){99,999};unsigned before=opens;
 assert(!write_catalog_snapshot_at(many,PET_P4_MINIAPP_CATALOG_MAX+1,"item-0","item-1",false,0,51,&result)&&opens==before&&result.generation==99);
 return 0;
}
'''


def test_in_place_catalog_generation_is_self_checked_and_torn_writes_are_rejected(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    for acceptance in (False,True):
        src=tmp_path/f'lean-{int(acceptance)}.c';exe=src.with_suffix('');src.write_text(_catalog_source(code)+MAIN)
        vendor=ROOT/'tests/vendor/cjson'
        subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined',
            '-DPET_P4_COMMIT_LEAN_IO=1',
            *(['-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_COMPONENT_BACKGROUND_ENABLED=1',
               '-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1','-DPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1'] if acceptance else []),
            '-I',str(ROOT/'main'),'-I',str(vendor),str(src),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],cwd=tmp_path,check=True,timeout=120)


def test_sprite_cleanup_memo_is_boot_local_positive_and_commit_order_unchanged(tmp_path):
    code=(ROOT/'main/pet_p4_miniapp.c').read_text()
    persist=function(code,'persist_sprite_pack')
    # Skip only for zero sprites when THIS boot already left that generation with zero sprite files.
    assert "skip_cleanup=pack&&pack->count==0&&memo_slot&&g_sprite_count_seen[slot][package_generation]==1" in persist
    assert persist.count('if(skip_cleanup)continue;')==1 and persist.index('if(skip_cleanup)continue;')<persist.index('pet_file_cleanup_prepare(')
    assert "if(ok&&memo_slot&&pack->count<=PET_P4_MINIAPP_SPRITE_MAX)g_sprite_count_seen[slot][package_generation]=(uint8_t)(pack->count+1);" in persist
    assert 'static uint8_t g_sprite_count_seen[PET_P4_MINIAPP_CATALOG_MAX][MINIAPP_PACKAGE_GENERATION_COUNT];' in code
    commit=code[code.index('bool pet_p4_miniapp_install_commit('):]
    assert commit.index('write_spare_package_json(')<commit.index('persist_sprite_pack(')<commit.index('load_catalog_runtime_item(')<commit.index('restore_component_runtime(')<commit.index('write_catalog_snapshot_at(')<commit.index('PET_STORAGE_DURABLE')
    loader=function(code,'load_catalog_snapshot')
    assert 'cJSON_ParseWithOpts(json,&parse_end,0)' in loader and 'catalog_check_verify(json,(size_t)(parse_end-json))' in loader
    writer=code[code.index('static bool write_catalog_snapshot_at('):code.index('/* Boot-only/non-yielding callers')]
    assert 'catalog_check_seal(json) && write_catalog_in_place(catalog_path, json, strlen(json), &may_have_changed)' in writer
    assert writer.index('"sequence"')<writer.index('"check"')<writer.index('"activeWidgetId"')
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_commit_lean_parent':
            assert config[section]['extends']=='env:esp32_p4_frame_exact_parent'
            assert config[section]['custom_p4_project_version']=='0.7.103-p4-r131-lean' and 'PET_P4_COMMIT_LEAN_IO=1' in flags
            assert not any(v in flags for v in ('TASK_PRIORITY','PROGRAM_EXECUTION','F_CPU','PERIPHERAL','BUDGET_US','SPIFFS_MAX_FILES','AFFINITY'))
        else:assert 'PET_P4_COMMIT_LEAN_IO=1' not in flags
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_storage_io_trace.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-fsyntax-only','-DPET_P4_COMMIT_LEAN_IO=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Lean commit IO requires' in result.stderr
    assert subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True).returncode==0
