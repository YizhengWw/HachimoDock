"""Real cold package loader/read adapters reject partial I/O and revocation."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_actual_loader_preserves_input_metadata_on_io_and_checksum_errors(tmp_path):
    code = (ROOT / "main/pet_p4_miniapp.c").read_text()
    reads = code[code.index("static bool read_file_kind("):
                 code.index("bool pet_p4_miniapp_installed_id(")]
    loader = code[code.index("static bool load_catalog_runtime_item("):
                  code.index("static bool validate_catalog_snapshot(")]
    src = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_json_source.h"
#include "pet_p4_file_read.h"
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define MINIAPP_WIDGET_JSON_MAX 128
#define MINIAPP_BUTTONS_JSON_MAX 128
#define PET_WIDGET_TRACE_SCOPE(name,kind) int name __attribute__((unused))=0
#define pet_widget_trace_scope_end(p) ((void)(p))
typedef struct {uint8_t count;size_t size;uint8_t bytes[128];} pet_p4_miniapp_sprite_pack_t;
typedef struct {char widget_id[48];unsigned slot,package_generation,widget_checksum,buttons_checksum,sprites_checksum,resource_state,image_count;} miniapp_catalog_item_t;
typedef struct {char widget_id[48];bool active;} miniapp_runtime_t;
static bool current=true,error_read,parse_fail;
static unsigned opens,reads_count,closes,alloc_count,alloc_live,fail_alloc;
static unsigned fault_file,fault_kind;
static bool storage_work_current(void){return current;}
static void set_error(char *out,size_t n,const char *text){snprintf(out,n,"%s",text);}
static void *alloc(size_t n,size_t size){alloc_count++;if(alloc_count==fail_alloc)return NULL;void *p=calloc(n,size);if(p)alloc_live++;return p;}
static void release(void *p){if(p){assert(alloc_live);alloc_live--;free(p);}}
static FILE *open_read(const char *path,const char *mode){
 opens++;error_read=false;
 if(opens==fault_file&&fault_kind==1){errno=EIO;return NULL;}
 FILE *f=fopen(path,mode);if(opens==fault_file&&fault_kind==4)current=false;return f;
}
static size_t read_data(void *out,size_t size,size_t n,FILE *f){
 reads_count++;
 if(opens==fault_file&&fault_kind==2){error_read=true;return fread(out,size,3,f);}
 size_t got=fread(out,size,n,f);if(opens==fault_file&&fault_kind==5)current=false;return got;
}
static int read_error(FILE *f){return error_read?1:ferror(f);}
static int close_read(FILE *f){closes++;int rc=fclose(f);if(opens==fault_file&&fault_kind==3){errno=EIO;return EOF;}return rc;}
#define fopen open_read
#define fread read_data
#define fclose close_read
#define ferror read_error
''' + reads + r'''
#undef fopen
#undef fread
#undef fclose
#undef ferror
static uint32_t miniapp_checksum(const char *data,size_t n){uint32_t sum=0;for(size_t i=0;i<n;i++)sum+=(unsigned char)data[i];return sum;}
static uint32_t sprite_pack_checksum(const pet_p4_miniapp_sprite_pack_t *p){return miniapp_checksum((const char*)p->bytes,p->size);}
static void catalog_slot_path(size_t slot,uint8_t generation,bool buttons,bool tmp,char *out,size_t n){
 (void)slot;(void)generation;(void)tmp;snprintf(out,n,"%s",buttons?"buttons.json":"widget.json");
}
static bool load_sprite_pack(const char *json,size_t slot,uint8_t generation,pet_p4_miniapp_sprite_pack_t *out,char *error,size_t n){
 (void)json;(void)slot;(void)generation;(void)error;(void)n;
 if(!read_binary_file("sprite.bin",out->bytes,sizeof(out->bytes),&out->size))return false;
 out->count=1;return true;
}
static bool parse_runtime(miniapp_runtime_t *out,const char *id,const char *widget,const char *buttons,
 const pet_p4_miniapp_sprite_pack_t *sprites,char *error,size_t n){
 (void)error;(void)n;if(parse_fail)return false;
 assert(!strcmp(widget,"WIDGET")&&!strcmp(buttons,"BUTTONS")&&sprites->size==5);
 strcpy(out->widget_id,id);out->active=true;return true;
}
static bool pet_p4_builtin_is_bootstrap(const char *id,uint32_t sum){(void)id;(void)sum;return false;}
#define calloc alloc
#define free release
''' + loader + r'''
#undef calloc
#undef free
static void put(const char *path,const char *text){FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void reset(void){assert(!alloc_live);opens=reads_count=closes=alloc_count=fail_alloc=0;current=true;error_read=parse_fail=false;fault_file=fault_kind=0;}
int main(void){
 put("widget.json","WIDGET");put("buttons.json","BUTTONS");put("sprite.bin","ASSET");
 miniapp_catalog_item_t item={.widget_id="fixture",.slot=0,.package_generation=1,.resource_state=1,.image_count=1,
  .widget_checksum=miniapp_checksum("WIDGET",6),.buttons_checksum=miniapp_checksum("BUTTONS",7),.sprites_checksum=miniapp_checksum("ASSET",5)};
 miniapp_runtime_t next={0};char error[128]={0};
 assert(load_catalog_runtime_item(&item,&next,false,error,sizeof(error))&&opens==3&&closes==3&&next.active);
 unsigned max_alloc=alloc_count;
 for(unsigned a=1;a<=max_alloc;a++){reset();fail_alloc=a;miniapp_catalog_item_t copy=item;
  assert(!load_catalog_runtime_item(&copy,&next,false,error,sizeof(error))&&alloc_live==0&&opens==0&&!memcmp(&item,&copy,sizeof(item)));}
 for(unsigned file=1;file<=3;file++)for(unsigned kind=1;kind<=5;kind++){
  reset();fault_file=file;fault_kind=kind;miniapp_catalog_item_t copy=item;
  assert(!load_catalog_runtime_item(&copy,&next,false,error,sizeof(error)));
  assert(alloc_live==0&&!memcmp(&item,&copy,sizeof(item))&&opens==file);
  assert(closes==opens-(kind==1));
 }
 reset();miniapp_catalog_item_t copy=item;copy.sprites_checksum++;
 assert(!load_catalog_runtime_item(&copy,&next,false,error,sizeof(error))&&alloc_live==0);
 reset();parse_fail=true;copy=item;assert(!load_catalog_runtime_item(&copy,&next,false,error,sizeof(error))&&!memcmp(&copy,&item,sizeof(item)));
 reset();current=false;copy=item;assert(!load_catalog_runtime_item(&copy,&next,false,error,sizeof(error))&&opens==0&&alloc_live==0);
 reset();copy=item;assert(load_catalog_runtime_item(&copy,&next,false,error,sizeof(error))&&next.active&&alloc_live==0);
 /* Raw/encoded NUL in JSON cannot be hidden by strlen or accepted checksums.
  * Old identity text and arbitrary binary asset bytes retain their semantics. */
 reset();FILE *bad=fopen("widget.json","wb");assert(bad);const char hidden[]={ 'W','I','D','G','E','T',0,'x' };assert(fwrite(hidden,1,sizeof(hidden),bad)==sizeof(hidden));fclose(bad);
 copy=item;assert(!load_catalog_runtime_item(&copy,&next,false,error,sizeof(error))&&opens==1&&alloc_live==0&&!memcmp(&copy,&item,sizeof(item)));
 char text[64];assert(read_file("widget.json",text,sizeof(text))&&!strcmp(text,"WIDGET"));
 uint8_t binary[64]={0};size_t binary_len=0;assert(read_binary_file("widget.json",binary,sizeof(binary),&binary_len)&&binary_len==sizeof(hidden)&&!memcmp(binary,hidden,sizeof(hidden)));
 put("widget.json","{\"value\":\"A\\u0000B\"}");assert(!read_json_file("widget.json",text,sizeof(text)));
 put("widget.json","{\"value\":\"A\\\\u0000B\"}");assert(read_json_file("widget.json",text,sizeof(text)));
 bad=fopen("widget.json","wb");assert(bad);const uint8_t invalid_utf8[]={'{','"','v','"',':','"',0xc0,0x80,'"','}'};
 assert(fwrite(invalid_utf8,1,sizeof(invalid_utf8),bad)==sizeof(invalid_utf8));fclose(bad);
 assert(!read_json_file("widget.json",text,sizeof(text)));
 assert(read_file("widget.json",text,sizeof(text))&&!memcmp(text,invalid_utf8,sizeof(invalid_utf8)));
 assert(read_binary_file("widget.json",binary,sizeof(binary),&binary_len)&&binary_len==sizeof(invalid_utf8)&&!memcmp(binary,invalid_utf8,binary_len));
 char deep[80],large_text[128];memset(deep,'[',33);deep[33]='0';memset(deep+34,']',33);deep[67]=0;
 put("widget.json",deep);assert(!read_json_file("widget.json",large_text,sizeof(large_text)));
 assert(read_file("widget.json",large_text,sizeof(large_text))&&!strcmp(large_text,deep));
 char boundary[128];memset(boundary,' ',sizeof(boundary)-1);boundary[0]='{';boundary[1]='}';boundary[127]=0;
 put("widget.json",boundary);assert(read_json_file("widget.json",large_text,sizeof(large_text))&&strlen(large_text)==127);
 assert(!read_json_file("widget.json",large_text,sizeof(large_text)-1));
 bad=fopen("sprite.bin","wb");assert(bad);assert(fwrite(binary,1,sizeof(binary),bad)==sizeof(binary));fclose(bad);
 assert(read_binary_file("sprite.bin",binary,sizeof(binary),&binary_len)&&binary_len==sizeof(binary));
 assert(!read_binary_file("sprite.bin",binary,sizeof(binary)-1,&binary_len));
 puts("actual package loader: all per-file faults rejected");
}
'''
    path = tmp_path / "files.c"
    path.write_text(src)
    exe = tmp_path / "files"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    '-I',str(ROOT/'main'),str(path), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], cwd=tmp_path, check=True)
