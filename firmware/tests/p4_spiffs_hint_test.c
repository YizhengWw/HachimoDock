/* Installed, unmodified SDK SPIFFS on host RAM flash. Preprocessor symbol
 * redirection substitutes for GNU wrap on Apple ld; separate final-P4-ELF
 * audit required. No physical flash, user directory, USB or audio. */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "spiffs.h"
#include "spiffs_nucleus.h"
#include "pet_p4_spiffs_hint.c" /* Private corruption test, not a wire API. */
_Thread_local unsigned hint_test_lock_depth;
static unsigned sdk_lock,reads,fail_reads,real_calls;
static uint8_t medium[2*1024*1024],work[1024],fds[2048],cache[8192];
static spiffs fs;
int64_t esp_timer_get_time(void){assert(!hint_test_lock_depth);struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));errno=EOVERFLOW;return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
void spiffs_api_lock(spiffs *f){assert(f==&fs && !sdk_lock && !hint_test_lock_depth);sdk_lock=1;}
void spiffs_api_unlock(spiffs *f){assert(f==&fs && sdk_lock && !hint_test_lock_depth);sdk_lock=0;}
static s32_t rd(spiffs *f,u32_t addr,u32_t size,u8_t *dst){(void)f;assert(!hint_test_lock_depth);assert(addr<=sizeof(medium) && size<=sizeof(medium)-addr);reads++;if(fail_reads){errno=EIO;return -12345;}memcpy(dst,medium+addr,size);return SPIFFS_OK;}
/* NOR programming is bitwise AND: SPIFFS deliberately writes 0xff over old
 * zero bits in blind-write mode. It does not require an impossible 0->1. */
static s32_t wr(spiffs *f,u32_t addr,u32_t size,u8_t *src){(void)f;assert(!hint_test_lock_depth);assert(addr<=sizeof(medium) && size<=sizeof(medium)-addr);for(u32_t i=0;i<size;i++)medium[addr+i]&=src[i];return SPIFFS_OK;}
static s32_t erase(spiffs *f,u32_t addr,u32_t size){(void)f;assert(!hint_test_lock_depth);assert(addr<=sizeof(medium) && size<=sizeof(medium)-addr);memset(medium+addr,255,size);return SPIFFS_OK;}
static s32_t lookup(const char *name,spiffs_page_ix *pix){
    spiffs_api_lock(&fs);real_calls++;errno=E2BIG;
    s32_t r=__wrap_spiffs_object_find_object_index_header_by_name(&fs,(const u8_t*)name,pix);
    assert(errno==(fail_reads?EIO:E2BIG));
    spiffs_api_unlock(&fs);return r;
}
static void write_file(const char *name,unsigned value){
    spiffs_file f=SPIFFS_open(&fs,name,SPIFFS_CREAT|SPIFFS_TRUNC|SPIFFS_RDWR,0);assert(f>=0);
    assert(SPIFFS_write(&fs,f,&value,sizeof(value))==sizeof(value));assert(SPIFFS_close(&fs,f)==0);
}
static unsigned read_file(const char *name){
    unsigned value=0;spiffs_file f=SPIFFS_open(&fs,name,SPIFFS_RDONLY,0);assert(f>=0);
    assert(SPIFFS_read(&fs,f,&value,sizeof(value))==sizeof(value));assert(SPIFFS_close(&fs,f)==0);return value;
}
static void mount(void){
    spiffs_config cfg={.phys_size=sizeof(medium),.phys_erase_block=4096,.log_block_size=4096,.log_page_size=512,
      .hal_read_f=rd,.hal_write_f=wr,.hal_erase_f=erase};
    assert(SPIFFS_mount(&fs,&cfg,work,fds,sizeof(fds),cache,sizeof(cache),NULL)==0);
}
int main(void){
    memset(medium,255,sizeof(medium));
    spiffs_config cfg={.phys_size=sizeof(medium),.phys_erase_block=4096,.log_block_size=4096,.log_page_size=512,
      .hal_read_f=rd,.hal_write_f=wr,.hal_erase_f=erase};
    assert(SPIFFS_mount(&fs,&cfg,work,fds,sizeof(fds),cache,sizeof(cache),NULL)<0);
    assert(!SPIFFS_format(&fs));mount();
    for(unsigned n=0;n<120;n++){char name[32];snprintf(name,sizeof(name),"/record%03u",n);write_file(name,n);}
    for(unsigned n=0;n<120;n++){char name[32];snprintf(name,sizeof(name),"/record%03u",n);assert(read_file(name)==n);}
    spiffs_page_ix pix=65535;assert(!lookup("/record019",&pix));
    uint32_t hash;uint8_t length;assert(key((u8_t*)"/record019",&hash,&length));
    unsigned set=hash&(SETS-1),slot=WAYS;
    for(unsigned w=0;w<WAYS;w++)if(hints[set][w].valid && hints[set][w].hash==hash)slot=w;
    assert(slot<WAYS);
    /* Stale/hash-colliding coordinates at another file cannot supply success. */
    spiffs_page_ix other;assert(!lookup("/record099",&other));
    hints[set][slot].block=(uint16_t)SPIFFS_BLOCK_FOR_PAGE(&fs,other);
    hints[set][slot].entry=(uint16_t)SPIFFS_OBJ_LOOKUP_ENTRY_FOR_PAGE(&fs,other);
    assert(!lookup("/record019",&other) && other==pix);assert(read_file("/record019")==19);
    hints[set][slot].block=65535;hints[set][slot].entry=65535;
    assert(!lookup("/record019",&other) && other==pix);
    assert(!SPIFFS_rename(&fs,"/record019","/renamed"));
    assert(lookup("/record019",&other)==SPIFFS_ERR_NOT_FOUND);
    assert(read_file("/renamed")==19);write_file("/renamed",999);assert(read_file("/renamed")==999);
    assert(!SPIFFS_remove(&fs,"/renamed"));assert(lookup("/renamed",&other)==SPIFFS_ERR_NOT_FOUND);
    /* Newly created name is immediately visible; no negative cache. */
    write_file("/renamed",77);assert(read_file("/renamed")==77);
    assert(!lookup("/renamed",&other));spiffs_cache_init(&fs);fail_reads=1;
    assert(lookup("/renamed",&other)==-12345);fail_reads=0;assert(read_file("/renamed")==77);
    for(unsigned n=0;n<900;n++){
        char name[32];snprintf(name,sizeof(name),"/record%03u",n%120);
        if(n%120==19)continue;
        write_file(name,n);assert(read_file(name)==n);
    }
    SPIFFS_unmount(&fs);mount();assert(read_file("/renamed")==77);
    assert(lookup("/record019",&other)==SPIFFS_ERR_NOT_FOUND);
    pet_spiffs_hint_stats_t s;pet_spiffs_hint_stats(&s);pet_spiffs_hint_stats(NULL);
    assert(s.calls>=real_calls && s.positives && s.negatives && s.errors==1);
    assert(s.candidates && s.applied==(PET_P4_SPIFFS_HINT_APPLY?s.candidates:0));
    assert(s.calls==s.positives+s.negatives+s.errors);
    printf("apply=%d calls=%u positive=%u negative=%u errors=%u candidates=%u applied=%u ramFlashReads=%u\n",
      PET_P4_SPIFFS_HINT_APPLY,s.calls,s.positives,s.negatives,s.errors,s.candidates,s.applied,reads);
    SPIFFS_unmount(&fs);return 0;
}
