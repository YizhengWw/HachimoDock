"""Native exact-byte no-op admission; no device, user data, cloud or audio."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def run(tmp_path,code,trace=False):
    source=tmp_path/'exact.c';binary=tmp_path/'exact';source.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined',*(['-DPET_P4_FILE_EXACT_TRACE_ENABLE=1'] if trace else []),
        '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True,timeout=20)

def test_exact_file_segments_eof_short_bytes_and_owner_revocation(tmp_path):
    _segments(tmp_path,False)

def test_exact_file_traced_path_keeps_every_byte_eof_and_revocation_case(tmp_path):
    _segments(tmp_path,True)

def _segments(tmp_path,trace):
    run(tmp_path,r'''
#include "pet_p4_file_exact.h"
#include <assert.h>
static unsigned calls,deny;
static bool current(void){return !deny||++calls<deny;}
static void put(const void*b,size_t n){FILE*f=fopen("test.bin","wb");assert(f&&fwrite(b,1,n,f)==n);assert(!fclose(f));}
int main(void){
 uint8_t b[1024];for(unsigned i=0;i<sizeof(b);i++)b[i]=(uint8_t)(i*37);
 for(unsigned n=0;n<=sizeof(b);n++){
  put(b,n);for(unsigned k=0;k<=n;k+=17)assert(pet_file_exact("test.bin",b,k,b+k,n-k,current)==1);
  if(n)assert(pet_file_exact("test.bin",b,n-1,NULL,0,current)==0);
  if(n<sizeof(b))assert(pet_file_exact("test.bin",b,n+1,NULL,0,current)==0);
 }
 put(b,sizeof(b));b[777]^=1;assert(!pet_file_exact("test.bin",b,8,b+8,sizeof(b)-8,current));b[777]^=1;
 for(deny=1;deny<8;deny++){calls=0;assert(pet_file_exact("test.bin",b,8,b+8,sizeof(b)-8,current)==-1);}
 deny=0;assert(!pet_file_exact("missing.bin",b,1,NULL,0,current));
 assert(!pet_file_exact("test.bin",NULL,1,NULL,0,current));return 0;
}
''',trace=trace)

def test_exact_file_numeric_spans_close_once_on_success_mismatch_and_cancel(tmp_path):
    run(tmp_path,r'''
#define PET_P4_FILE_EXACT_TRACE_ENABLE 1
#define PET_WIDGET_TRACE_TEST 1
#include "pet_p4_file_exact.h"
#include <assert.h>
static unsigned stages[PET_WTRACE_STAGE_COUNT],depth,calls,deny;
pet_widget_trace_scope_t pet_widget_trace_scope_begin(pet_widget_trace_stage_t stage){
 assert(!depth&&stage>0&&stage<PET_WTRACE_STAGE_COUNT);depth=1;stages[stage]++;
 return (pet_widget_trace_scope_t){.stage=stage,.active=true};
}
void pet_widget_trace_scope_end(pet_widget_trace_scope_t *scope){
 if(!scope->active)return;assert(depth==1);depth=0;scope->active=false;
}
static bool current(void){return !deny||++calls<deny;}
int main(void){
 unsigned char data[1024];for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)i;
 FILE *file=fopen("sample.bin","wb");assert(file&&fwrite(data,1,sizeof(data),file)==sizeof(data)&&!fclose(file));
 assert(pet_file_exact("sample.bin",data,sizeof(data),NULL,0,current)==1&&!depth);
 assert(stages[PET_WTRACE_FS_OPEN]==1&&stages[PET_WTRACE_FS_DATA]==5&&stages[PET_WTRACE_FS_CLOSE]==1);
 data[0]^=1;assert(!pet_file_exact("sample.bin",data,sizeof(data),NULL,0,current));data[0]^=1;
 assert(stages[PET_WTRACE_FS_OPEN]==2&&stages[PET_WTRACE_FS_CLOSE]==2&&!depth);
 for(deny=2;deny<7;deny++){
  calls=0;unsigned opens=stages[PET_WTRACE_FS_OPEN],closes=stages[PET_WTRACE_FS_CLOSE];
  assert(pet_file_exact("sample.bin",data,sizeof(data),NULL,0,current)==-1&&!depth);
  assert(stages[PET_WTRACE_FS_OPEN]==opens+1&&stages[PET_WTRACE_FS_CLOSE]==closes+1);
 }
 return 0;
}
''')

def test_physical_verify_bypasses_stale_cache_without_adopting_another_task(tmp_path):
    for apply in (0,1):
        binary=tmp_path/f'physical-{apply}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-fsanitize=address,undefined','-DPET_STORAGE_IO_HOST_TEST=1','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1',
            '-DPET_P4_STORAGE_PRESERVE_UNCHANGED=1','-DPET_P4_STORAGE_PAGE_CACHE_ENABLE=1',
            f'-DPET_P4_STORAGE_PAGE_CACHE_APPLY={apply}','-DPET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH=1',
            '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),str(ROOT/'tests/p4_storage_page_cache_test.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=20)

def test_actual_unchanged_helper_compares_all_resources_preserving_live_state(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    code=r'''
#include "pet_p4_file_exact.h"
#include <assert.h>
#define MINIAPP_SPRITE_FILE_HEADER_SIZE 8
typedef struct {unsigned frame_width,frame_height,frames,fps,data_offset,data_length;} pet_p4_miniapp_sprite_t;
typedef struct {unsigned count,data_length;uint8_t pixels[1024];pet_p4_miniapp_sprite_t items[4];} pack_t;
typedef struct {char widget_id[48],title[64];unsigned resource_state,slot,package_generation;uint32_t widget_checksum,buttons_checksum,sprites_checksum;} miniapp_catalog_item_t;
typedef struct {bool active;char widget_id[48];struct {char title[64];}view;} miniapp_runtime_t;
typedef struct {bool preserve_unchanged,repair_resources;char widget_id[48],widget_json[1024],buttons_json[1024];size_t widget_len,buttons_len;pack_t sprites;} staging_t;
typedef struct {bool active;} pet_storage_physical_scope_t;
static bool audio,owner=true,physical,deny_begin,revoke_finish;
static unsigned model_lock,begins,finishes;
static miniapp_runtime_t g_runtime={.active=true,.widget_id="owned",.view={.title="Title"}};
static staging_t g_staging={.preserve_unchanged=true,.widget_id="owned",.widget_json="{valid}",.widget_len=7,.buttons_json="[]",.buttons_len=2};
static miniapp_catalog_item_t g_catalog[1]={{.widget_id="owned",.title="Title",.resource_state=1,.widget_checksum=7,.buttons_checksum=2}};
static size_t g_catalog_count=1,g_catalog_selected;
static uint32_t g_image_revision=8;
static bool g_builtin_sync_in_progress;
static bool storage_work_current(void){return owner;}
static bool storage_audio_busy(void){return audio;}
static void miniapp_runtime_lock(void){assert(!model_lock);model_lock++;}
static void miniapp_runtime_unlock(void){assert(model_lock==1);model_lock--;}
static int catalog_find(const miniapp_catalog_item_t*i,size_t n,const char*s){return n&&!strcmp(i->widget_id,s)?0:-1;}
// Deliberate checksum collision: only exact bytes may authorize success.
static uint32_t miniapp_checksum(const char*s,size_t n){(void)s;return (uint32_t)n;}
static uint32_t sprite_pack_checksum(const pack_t*p){return p->count?123:0;}
static bool pet_storage_physical_begin(pet_storage_physical_scope_t*s){assert(!model_lock);begins++;if(deny_begin)return false;assert(!physical);physical=s->active=true;return true;}
static bool pet_storage_physical_finish(pet_storage_physical_scope_t*s){assert(!model_lock&&physical&&s->active);finishes++;physical=s->active=false;if(revoke_finish)owner=false;return true;}
static void catalog_slot_path(unsigned s,unsigned g,bool buttons,bool tmp,char*p,size_t n){(void)s;(void)g;(void)tmp;snprintf(p,n,"%s.bin",buttons?"buttons":"widget");}
static void catalog_sprite_path(unsigned s,unsigned g,unsigned i,bool tmp,char*p,size_t n){(void)s;(void)g;(void)i;(void)tmp;snprintf(p,n,"sprite.bin");}
static void put(const char*p,const void*b,size_t n){FILE*f=fopen(p,"wb");assert(f&&fwrite(b,1,n,f)==n);assert(!fclose(f));}
'''+function(text,'unchanged_storage_current')+'\n'+function(text,'preserve_unchanged_package')+r'''
int main(void){
 miniapp_runtime_t next=g_runtime,original=g_runtime;
 put("widget.bin",g_staging.widget_json,7);put("buttons.bin",g_staging.buttons_json,2);
 assert(preserve_unchanged_package(&next)==1&&begins==1&&finishes==1);
 assert(!memcmp(&original,&g_runtime,sizeof(original))&&g_image_revision==8&&!physical);
 g_staging.widget_json[1]^=1;assert(!preserve_unchanged_package(&next));g_staging.widget_json[1]^=1;
 put("buttons.bin","[]x",3);assert(!preserve_unchanged_package(&next));put("buttons.bin","[]",2);
 g_staging.sprites.count=1;g_staging.sprites.data_length=600;g_catalog[0].sprites_checksum=123;
 pet_p4_miniapp_sprite_t*s=&g_staging.sprites.items[0];*s=(pet_p4_miniapp_sprite_t){16,16,1,8,0,600};
 uint8_t image[608]={'P','4','S','1',16,16,1,8};
 for(unsigned i=0;i<600;i++)image[i+8]=g_staging.sprites.pixels[i]=(uint8_t)(i*17);
 put("sprite.bin",image,sizeof(image));assert(preserve_unchanged_package(&next)==1);
 image[0]^=1;put("sprite.bin",image,sizeof(image));assert(!preserve_unchanged_package(&next));image[0]^=1;
 image[408]^=1;put("sprite.bin",image,sizeof(image));assert(!preserve_unchanged_package(&next));image[408]^=1;
 s->fps=0;put("sprite.bin",image+8,600);assert(preserve_unchanged_package(&next)==1);
 s->data_length=601;assert(!preserve_unchanged_package(&next));s->data_length=600;
 g_staging.repair_resources=true;assert(!preserve_unchanged_package(&next));g_staging.repair_resources=false;
 g_builtin_sync_in_progress=true;assert(!preserve_unchanged_package(&next));g_builtin_sync_in_progress=false;
 for(unsigned state=0;state<4;state++)if(state!=1){g_catalog[0].resource_state=state;assert(!preserve_unchanged_package(&next));}g_catalog[0].resource_state=1;
 g_runtime.active=false;assert(!preserve_unchanged_package(&next));g_runtime.active=true;
 g_catalog_selected=1;assert(!preserve_unchanged_package(&next));g_catalog_selected=0;
 g_staging.preserve_unchanged=false;assert(!preserve_unchanged_package(&next));g_staging.preserve_unchanged=true;
 deny_begin=true;assert(preserve_unchanged_package(&next)==-1);deny_begin=false;
 audio=true;assert(preserve_unchanged_package(&next)==-1);audio=false;
 revoke_finish=true;assert(preserve_unchanged_package(&next)==-1&&!physical);owner=true;
 assert(!memcmp(&original,&g_runtime,sizeof(original))&&g_image_revision==8&&!physical&&!model_lock);return 0;
}
'''
    run(tmp_path,code)
    commit=function(text,'pet_p4_miniapp_install_commit')
    assert commit.index('parse_runtime(')<commit.index('preserve_unchanged_package(')<commit.index('background_prepare_replace(')<commit.index('write_spare_package_json(')
    branch=commit.split('if(unchanged==1){',1)[1].split('#endif',1)[0]
    assert all(word not in branch for word in ('g_runtime =','g_catalog_selected =','g_image_revision++','background_','write_','persist_'))
