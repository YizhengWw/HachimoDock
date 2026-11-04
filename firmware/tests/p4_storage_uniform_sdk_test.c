/* Unmodified installed SDK, RAM NOR only. Not physical flash/performance. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
unsigned io_meta_depth;
#include "pet_p4_storage_io_trace.c"
#include "spiffs_nucleus.h"
#define TEST_CACHE_APPLY (PET_P4_STORAGE_UNIFORM_CACHE_APPLY || PET_P4_STORAGE_PAGE_CACHE_APPLY)
static uint8_t medium[2*1024*1024],work[1024],fds[4096],cache[4320];
static spiffs fs;
static unsigned sdk_lock,reads;
static bool fail_reads;
static int64_t clock_us=100;
int64_t esp_timer_get_time(void){assert(!io_meta_depth);return clock_us;}
void *xTaskGetCurrentTaskHandle(void){assert(!io_meta_depth);return (void*)3;}
void spiffs_api_lock(spiffs *f){assert(f==&fs&&!sdk_lock&&!io_meta_depth);sdk_lock=1;}
void spiffs_api_unlock(spiffs *f){assert(f==&fs&&sdk_lock&&!io_meta_depth);sdk_lock=0;}
s32_t __real_spiffs_api_read(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){
 (void)f;assert(!io_meta_depth&&a<=sizeof(medium)&&n<=sizeof(medium)-a);clock_us++;reads++;
 if(fail_reads){errno=EIO;return -12345;}memcpy(p,medium+a,n);return 0;
}
s32_t __real_spiffs_api_write(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){
 (void)f;assert(!io_meta_depth&&a<=sizeof(medium)&&n<=sizeof(medium)-a);clock_us++;
 for(uint32_t i=0;i<n;i++)medium[a+i]&=p[i];return 0;
}
s32_t __real_spiffs_api_erase(spiffs *f,uint32_t a,uint32_t n){
 (void)f;assert(!io_meta_depth&&a<=sizeof(medium)&&n<=sizeof(medium)-a);clock_us++;memset(medium+a,255,n);return 0;
}
static spiffs_config config(void){return (spiffs_config){.phys_size=sizeof(medium),.phys_erase_block=4096,.log_block_size=4096,.log_page_size=512,
 .hal_read_f=__wrap_spiffs_api_read,.hal_write_f=__wrap_spiffs_api_write,.hal_erase_f=__wrap_spiffs_api_erase};}
static void mount(void){spiffs_config c=config();assert(!SPIFFS_mount(&fs,&c,work,fds,sizeof(fds),cache,sizeof(cache),NULL));}
static void write_file(const char *name,unsigned value){
 spiffs_file f=SPIFFS_open(&fs,name,SPIFFS_CREAT|SPIFFS_TRUNC|SPIFFS_RDWR,0);assert(f>=0);
 assert(SPIFFS_write(&fs,f,&value,sizeof(value))==sizeof(value));assert(!SPIFFS_close(&fs,f));
}
static unsigned read_file(const char *name){
 unsigned value=0;spiffs_file f=SPIFFS_open(&fs,name,SPIFFS_RDONLY,0);assert(f>=0);
 assert(SPIFFS_read(&fs,f,&value,sizeof(value))==sizeof(value));assert(!SPIFFS_close(&fs,f));return value;
}
int main(void){
 memset(medium,255,sizeof(medium));spiffs_config c=config();assert(SPIFFS_mount(&fs,&c,work,fds,sizeof(fds),cache,sizeof(cache),NULL)<0);
 assert(!SPIFFS_format(&fs));mount();
 assert(pet_storage_io_register(PET_STORAGE_IO_MAIN,(void*)1));assert(pet_storage_io_register(PET_STORAGE_IO_SAVE,(void*)2));assert(pet_storage_io_register(PET_STORAGE_IO_PROTOCOL,(void*)3));
 uint32_t g=pet_storage_io_begin(5000000);assert(g);
 for(unsigned n=0;n<120;n++){char name[32];snprintf(name,sizeof(name),"/item%03u",n);write_file(name,n);}
 for(unsigned n=0;n<120;n++){char name[32];snprintf(name,sizeof(name),"/item%03u",n);assert(read_file(name)==n);}
 assert(!SPIFFS_rename(&fs,"/item019","/renamed"));assert(read_file("/renamed")==19);
 spiffs_stat st;assert(SPIFFS_stat(&fs,"/item019",&st)==SPIFFS_ERR_NOT_FOUND);
 write_file("/renamed",999);assert(read_file("/renamed")==999);assert(!SPIFFS_remove(&fs,"/renamed"));
 assert(SPIFFS_stat(&fs,"/renamed",&st)==SPIFFS_ERR_NOT_FOUND);write_file("/renamed",77);assert(read_file("/renamed")==77);
 for(unsigned n=0;n<900;n++){
   char name[32];snprintf(name,sizeof(name),"/item%03u",n%120);if(n%120==19)continue;
   write_file(name,n);assert(read_file(name)==n);
 }
 assert(pet_storage_io_stop(g));pet_storage_io_counter_t out;assert(pet_storage_io_read(g,3,0,&out));
 assert(!out.errors&&out.calls>0);assert(TEST_CACHE_APPLY?out.cache_hits>0:out.cache_hits==0);
 /* Parent never remounts during the bounded cache window. Closed epochs do
  * not survive same-address unmount/remount or suppress a new miss failure. */
 SPIFFS_unmount(&fs);mount();g=pet_storage_io_begin(5000000);assert(g);spiffs_cache_init(&fs);fail_reads=true;
 assert(SPIFFS_stat(&fs,"/renamed",&st)==-12345);fail_reads=false;assert(read_file("/renamed")==77);
 assert(pet_storage_io_stop(g));SPIFFS_unmount(&fs);
 uint64_t hash=UINT64_C(14695981039346656037);for(unsigned i=0;i<sizeof(medium);i++){hash^=medium[i];hash*=UINT64_C(1099511628211);}
 printf("PASS original SDK COW/GC/rename/remove/recreate/remount/miss-error apply=%d physicalReads=%u hits=%u hash=%016llx\n",TEST_CACHE_APPLY,reads,out.cache_hits,(unsigned long long)hash);return 0;
}
