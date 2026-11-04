/* Actual private wrapper, adversarial copy interleavings, no real device IO. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
unsigned io_meta_depth;
static void *observed_copy(void *,const void *,size_t);
#define memcpy observed_copy
#include "pet_p4_storage_io_trace.c"
#undef memcpy
#if PET_P4_RESET_PHASE_TRACE
static unsigned record_enters,record_leaves;
pet_reset_phase_scope_t pet_reset_phase_enter(pet_reset_phase_t phase){assert(!io_meta_depth&&phase>=PET_PHASE_HAL_READ_META&&phase<=PET_PHASE_HAL_ERASE_NATIVE);record_enters++;return (pet_reset_phase_scope_t){.active=true};}
void pet_reset_phase_leave(pet_reset_phase_scope_t *scope){assert(!io_meta_depth&&scope->active);record_leaves++;scope->active=false;}
void pet_reset_phase_set(pet_reset_phase_t phase){assert(!io_meta_depth&&phase>=PET_PHASE_HAL_READ_META&&phase<=PET_PHASE_HAL_ERASE_NATIVE);}
#endif
static uint8_t medium[4*1024*1024],data[1024],nested[512];
static spiffs fs,other;
static int64_t clock_us=100;
static unsigned reads,writes,erases,hook;
static uint32_t target,collision,read_mutation;
static bool fail_read,fail_write,fail_erase;
static void *current_task=(void*)3;
int64_t esp_timer_get_time(void){assert(!io_meta_depth);return clock_us;}
void *xTaskGetCurrentTaskHandle(void){assert(!io_meta_depth);return current_task;}
s32_t __real_spiffs_api_read(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){
 assert(!io_meta_depth);reads++;clock_us++;
 if(fail_read||(uint64_t)a+n>sizeof(medium)){errno=EIO;return -91;}memcpy(p,medium+a,n);
 if(read_mutation){read_mutation=0;uint8_t zero=0;assert(!__wrap_spiffs_api_write(f,a,1,&zero));}
 return 0;
}
s32_t __real_spiffs_api_write(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){
 (void)f;assert(!io_meta_depth);writes++;clock_us++;
 if(a<sizeof(medium)&&n)medium[a]&=p[0];
 if(fail_write||(uint64_t)a+n>sizeof(medium)){errno=ENOSPC;return -92;}return 0;
}
s32_t __real_spiffs_api_erase(spiffs *f,uint32_t a,uint32_t n){
 (void)f;assert(!io_meta_depth);erases++;clock_us++;
 if(a<sizeof(medium))memset(medium+a,255,n<sizeof(medium)-a?n:sizeof(medium)-a);
 if(fail_erase){errno=EIO;return -93;}return 0;
}
static bool in_page_data(const void *p){uintptr_t q=(uintptr_t)p,base=(uintptr_t)page_bytes;return q>=base&&q-base<sizeof(page_bytes);}
static void *observed_copy(void *dst,const void *src,size_t size){
 assert(!io_meta_depth);unsigned action=hook;
 if(size==512&&((action==1&&in_page_data(src))||(action==2&&in_page_data(dst)))){
   hook=0;uint8_t zero=0;assert(!__wrap_spiffs_api_write(&fs,target,1,&zero));
   /* Force a colliding read/fill while the outer exact slot is pinned or
    * reserved. Its bytes must not be overwritten or published stale. */
   assert(!__wrap_spiffs_api_read(&fs,collision,512,nested));assert(!memcmp(nested,medium+collision,512));
 }else if(action==3&&size==512&&in_page_data(src)){
   hook=0;uint32_t g=io_status.generation;assert(pet_storage_io_stop(g));
   pet_storage_io_status_t s;assert(pet_storage_io_status(g,&s)&&!s.complete&&s.inflight);
   assert(!pet_storage_io_begin(1000));
 }
 return memcpy(dst,src,size);
}
static uint32_t window(void){if(io_status.generation)assert(pet_storage_io_stop(io_status.generation));uint32_t g=pet_storage_io_begin(5000000);assert(g);return g;}
static void read_at(uint32_t a){errno=EACCES;assert(!__wrap_spiffs_api_read(&fs,a,512,data)&&errno==EACCES);assert(!memcmp(data,medium+a,512));}
int main(void){
#if PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH
 unsigned occupancy[PAGE_CACHE_SETS]={0};
 for(uint32_t n=0;n<2048;n++)assert(++occupancy[page_set(n*4096)]<=PAGE_CACHE_WAYS);
#endif
 for(unsigned i=0;i<sizeof(medium);i++)medium[i]=(uint8_t)((i*37)^(i>>3)^0xa5);
 assert(pet_storage_io_register(PET_STORAGE_IO_MAIN,(void*)1));assert(pet_storage_io_register(PET_STORAGE_IO_SAVE,(void*)2));assert(pet_storage_io_register(PET_STORAGE_IO_PROTOCOL,(void*)3));
 uint32_t a=0,b=UINT32_MAX,c=UINT32_MAX;
 for(uint32_t n=512;n<sizeof(medium);n+=512)if(page_set(n)==page_set(a)){if(b==UINT32_MAX)b=n;else{c=n;break;}}
 assert(b!=UINT32_MAX&&c!=UINT32_MAX);target=a;collision=c;
 uint32_t g=window();unsigned before=reads;read_at(a);read_at(a);
 assert(reads==before+(PET_P4_STORAGE_PAGE_CACHE_APPLY?1:2));
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
 {
   pet_storage_physical_scope_t physical={0},nested_scope={0};
   assert(pet_storage_physical_begin(&physical));
   pet_storage_physical_scope_t copied=physical;
   assert(!pet_storage_physical_finish(&copied));
   assert(!pet_storage_physical_begin(&nested_scope));
   uint8_t original=medium[a];medium[a]^=1;
   before=reads;read_at(a);assert(reads==before+1&&data[0]==medium[a]);
   current_task=(void*)4;assert(!pet_storage_physical_finish(&physical));
   if(PET_P4_STORAGE_PAGE_CACHE_APPLY){
     before=reads;assert(!__wrap_spiffs_api_read(&fs,a,512,data));
     assert(reads==before&&data[0]==original); /* Other actor's scope unchanged. */
   }
   current_task=(void*)3;medium[a]=original;
   assert(pet_storage_physical_finish(&physical));
   assert(!pet_storage_physical_finish(&copied)&&!pet_storage_physical_finish(&physical));
   before=reads;read_at(a);assert(reads==before+(PET_P4_STORAGE_PAGE_CACHE_APPLY?0:1));
   assert(pet_storage_physical_begin(&nested_scope));assert(pet_storage_physical_finish(&nested_scope));
 }
#endif
#if PET_P4_RESET_PHASE_TRACE
 assert(record_enters==(PET_P4_STORAGE_PAGE_CACHE_APPLY&&!PET_P4_RESET_PHASE_CACHE_HITS?1:2));
 assert(record_leaves==record_enters);
#endif
 if(PET_P4_STORAGE_PAGE_CACHE_APPLY){
   page_cache_entry_t *full=NULL;
   for(unsigned w=0;w<PAGE_CACHE_WAYS;w++)if(page_entries[page_set(a)][w].generation==g&&page_entry_address(&page_entries[page_set(a)][w])==a)full=&page_entries[page_set(a)][w];
   assert(full);for(unsigned n=0;n<PET_PAGE_PIN_MAX;n++)assert(page_entry_pin(full));
   before=reads;read_at(a);assert(reads==before+1&&page_entry_pins(full)==PET_PAGE_PIN_MAX);
   for(unsigned n=0;n<PET_PAGE_PIN_MAX;n++)assert(page_entry_unpin(full));
   read_at(b);uint8_t expected[512];memcpy(expected,medium+a,512);hook=1;
   errno=EACCES;assert(!__wrap_spiffs_api_read(&fs,a,512,data));assert(!hook&&!memcmp(data,expected,512));
   read_at(a);assert(data[0]==0); /* Invalidation did not recycle pinned bytes. */
   assert(!__wrap_spiffs_api_erase(&fs,a,512));assert(!__wrap_spiffs_api_erase(&fs,c,512));hook=2;before=reads;
   errno=EACCES;assert(!__wrap_spiffs_api_read(&fs,a,512,data));assert(!hook&&data[0]==255);
   read_at(a);assert(data[0]==0&&reads>=before+3); /* Old fill never published. */
   hook=3;read_at(a);assert(!hook);
   pet_storage_io_status_t s;assert(pet_storage_io_status(g,&s)&&s.complete&&!s.inflight&&(s.flags&PET_STORAGE_IO_LATE_RETURN));
 }
 g=window();before=reads;read_at(a);assert(reads==before+1);
 fail_read=true;int result=__wrap_spiffs_api_read(&fs,a,512,data);assert(result==(PET_P4_STORAGE_PAGE_CACHE_APPLY?0:-91));fail_read=false;
 uint8_t zero=0;fail_write=true;assert(__wrap_spiffs_api_write(&fs,a+1,1,&zero)==-92);fail_write=false;read_at(a);assert(data[1]==0);
 fail_erase=true;assert(__wrap_spiffs_api_erase(&fs,a,512)==-93);fail_erase=false;read_at(a);assert(data[0]==255&&data[1]==255);
 assert(!__wrap_spiffs_api_erase(&fs,a,512));read_mutation=1;assert(!__wrap_spiffs_api_read(&fs,a,512,data));read_at(a);assert(data[0]==0);
 before=reads;assert(!__wrap_spiffs_api_read(&other,a,512,data)&&reads==before+1);
 before=reads;assert(!__wrap_spiffs_api_read(&fs,a+1,511,data)&&reads==before+1);assert(!memcmp(data,medium+a+1,511));
 clock_us+=5000001;before=reads;read_at(a);assert(reads==before+1);
 g=window();read_at(a);assert(!__wrap_spiffs_api_write(&fs,8192,8193,&zero));before=reads;read_at(a);read_at(a);assert(reads==before+2);
 g=window();read_at(a);fail_read=true;assert(__wrap_spiffs_api_read(&fs,512,512,data)==-91);fail_read=false;
 assert(!pet_storage_io_status(g,(pet_storage_io_status_t*)page_bytes));
 page_mutation=UINT64_MAX-1;assert(!__wrap_spiffs_api_write(&fs,a,1,&zero));before=reads;read_at(a);assert(reads==before+1);
 assert(pet_storage_io_stop(g));pet_storage_io_counter_t out;assert(pet_storage_io_read(g,3,0,&out)&&out.calls>0);
 for(unsigned s=0;s<PAGE_CACHE_SETS;s++)for(unsigned w=0;w<PAGE_CACHE_WAYS;w++)assert(!page_entry_pins(&page_entries[s][w])&&!page_entry_filling(&page_entries[s][w]));
 assert(!io_meta_depth&&writes&&erases);puts("PASS full-page exact bytes/pinned hit/fill/failed partial mutations/collisions/epochs/miss-error");return 0;
}
