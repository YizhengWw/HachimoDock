#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
unsigned io_meta_depth;
#include "pet_p4_storage_io_trace.c"
static uint8_t medium[16384],data[512];
static spiffs fs,other;
static int64_t now=100;
static unsigned reads,writes,erases;
static bool fail_read,fail_write,mutate_on_read,stop_on_read;
int64_t esp_timer_get_time(void){assert(!io_meta_depth);return now;}
void *xTaskGetCurrentTaskHandle(void){assert(!io_meta_depth);return (void*)3;}
s32_t __real_spiffs_api_write(spiffs *f,uint32_t a,uint32_t n,uint8_t *p);
s32_t __real_spiffs_api_read(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){
 assert(!io_meta_depth&&n<=sizeof(medium)&&a<=sizeof(medium)-n);reads++;now++;
 if(fail_read){errno=EIO;return -91;}memcpy(p,medium+a,n);
 if(mutate_on_read){mutate_on_read=false;uint8_t v=0;assert(!__wrap_spiffs_api_write(f,a,1,&v));}
 if(stop_on_read){stop_on_read=false;assert(pet_storage_io_stop(io_status.generation));}
 return 0;
}
s32_t __real_spiffs_api_write(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){
 (void)f;assert(!io_meta_depth);writes++;now++;
 if(n&&a<sizeof(medium))medium[a]&=p[0];
 if(fail_write){errno=ENOSPC;return -92;}return 0;
}
s32_t __real_spiffs_api_erase(spiffs *f,uint32_t a,uint32_t n){
 (void)f;assert(!io_meta_depth);erases++;now++;if(a<sizeof(medium))memset(medium+a,255,n<sizeof(medium)-a?n:sizeof(medium)-a);return 0;
}
static uint32_t window(void){
 if(io_status.generation)assert(pet_storage_io_stop(io_status.generation));
 uint32_t g=pet_storage_io_begin(5000000);assert(g);return g;
}
static void read_page(uint32_t a,uint8_t expected){
 memset(data,17,sizeof(data));errno=EACCES;assert(!__wrap_spiffs_api_read(&fs,a,512,data));
 assert(errno==EACCES);for(unsigned i=0;i<512;i++)assert(data[i]==expected);
}
int main(void){
 memset(medium,255,sizeof(medium));
 assert(pet_storage_io_register(PET_STORAGE_IO_MAIN,(void*)1));assert(pet_storage_io_register(PET_STORAGE_IO_SAVE,(void*)2));assert(pet_storage_io_register(PET_STORAGE_IO_PROTOCOL,(void*)3));
 uint32_t g=window();unsigned r=reads;read_page(0,255);read_page(0,255);
 assert(reads==r+(PET_P4_STORAGE_UNIFORM_CACHE_APPLY?1:2));
 /* Preserve all trailing bytes exactly, rather than discarding SDK markers. */
 medium[1020]=0;medium[1021]=7;medium[1022]=83;medium[1023]=19;
 r=reads;assert(!__wrap_spiffs_api_read(&fs,512,512,data));assert(!memcmp(data,medium+512,512));
 assert(!__wrap_spiffs_api_read(&fs,512,512,data));assert(!memcmp(data,medium+512,512));
 assert(reads==r+(PET_P4_STORAGE_UNIFORM_CACHE_APPLY?1:2));
 assert(pet_storage_io_stop(g));pet_storage_io_counter_t c;assert(pet_storage_io_read(g,3,0,&c));
 assert(c.calls==(PET_P4_STORAGE_UNIFORM_CACHE_APPLY?2:4)&&c.cache_hits==(PET_P4_STORAGE_UNIFORM_CACHE_APPLY?2:0));
 /* A new window never inherits cached bytes, including new physical errors. */
 g=window();fail_read=true;errno=EACCES;assert(__wrap_spiffs_api_read(&fs,0,512,data)==-91&&errno==EIO);fail_read=false;read_page(0,255);
 /* A cache hit may mask a newly introduced read fault within its documented
  * short lifetime. Neither control nor a miss repairs that fault. */
 fail_read=true;int result=__wrap_spiffs_api_read(&fs,0,512,data);assert(result==(PET_P4_STORAGE_UNIFORM_CACHE_APPLY?0:-91));fail_read=false;
 uint8_t zero=0;fail_write=true;errno=EACCES;assert(__wrap_spiffs_api_write(&fs,0,1,&zero)==-92&&errno==ENOSPC);fail_write=false;
 r=reads;assert(!__wrap_spiffs_api_read(&fs,0,512,data)&&data[0]==0&&data[1]==255&&reads==r+1);
 r=reads;assert(!__wrap_spiffs_api_read(&fs,0,512,data)&&reads==r+1); /* Nonuniform data never fills. */
 assert(!__wrap_spiffs_api_erase(&fs,0,4096));read_page(0,255);r=reads;
 assert(!__wrap_spiffs_api_read(&other,0,512,data)&&reads==r+1); /* Exact FS identity, not just address. */
 assert(!__wrap_spiffs_api_erase(&fs,0,4096));mutate_on_read=true;r=reads;
 assert(!__wrap_spiffs_api_read(&fs,0,512,data)&&data[0]==255);assert(!__wrap_spiffs_api_read(&fs,0,512,data)&&data[0]==0&&reads==r+2);
 assert(!__wrap_spiffs_api_erase(&fs,0,4096));stop_on_read=true;read_page(0,255);r=reads;read_page(0,255);assert(reads==r+1);
 g=window();read_page(0,255);now+=5000001;r=reads;read_page(0,255);assert(reads==r+1);
 g=window();read_page(0,255);assert(!__wrap_spiffs_api_erase(&fs,0,8193));r=reads;read_page(0,255);read_page(0,255);assert(reads==r+2); /* Large mutation fails cache closed, not IO. */
 g=window();read_page(0,255);uniform_mutation=UINT64_MAX-1;assert(!__wrap_spiffs_api_write(&fs,8192,1,&zero));r=reads;read_page(0,255);assert(reads==r+1);
 assert(!pet_storage_io_status(g,(pet_storage_io_status_t*)uniform_entries));
 assert(writes>=3&&erases>=4&&!io_meta_depth);puts("PASS uniform exact-byte coherence/failed-partial-write/erase/inflight/window/error semantics");return 0;
}
