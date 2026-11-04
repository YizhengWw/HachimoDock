/* Real private wrapper/core with local HAL, time and metadata-lock adapters.
 * No SDK/file/device IO, cache, program permission or physical performance claim. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
unsigned io_meta_depth;
#include "pet_p4_storage_io_trace.c"
static int64_t clock_us=100;
static uintptr_t task_id=1;
static unsigned reads,writes,erases;
static spiffs fs;
static spiffs *observed_fs;
static uint32_t observed_address,observed_size;
static uint8_t *observed_buffer;
static s32_t hal_result;
static bool stop_in_hal;
int64_t esp_timer_get_time(void){assert(!io_meta_depth);errno=ERANGE;return clock_us;}
void *xTaskGetCurrentTaskHandle(void){assert(!io_meta_depth);errno=ERANGE;return (void*)task_id;}
static s32_t hal(spiffs *f,uint32_t address,uint32_t size,uint8_t *p){
 assert(!io_meta_depth&&errno==EACCES);observed_fs=f;observed_address=address;observed_size=size;observed_buffer=p;
 if(stop_in_hal){
  uint32_t g=io_status.generation;assert(pet_storage_io_stop(g));
  pet_storage_io_status_t status;assert(pet_storage_io_status(g,&status)&&!status.active&&!status.complete&&status.inflight==1);
  assert(!pet_storage_io_begin(1000));pet_storage_io_counter_t out={.calls=77};assert(!pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_READ,&out)&&out.calls==77);
 }
 clock_us+=17;errno=EIO;return hal_result;
}
s32_t __real_spiffs_api_read(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){reads++;return hal(f,a,n,p);}
s32_t __real_spiffs_api_write(spiffs *f,uint32_t a,uint32_t n,uint8_t *p){writes++;return hal(f,a,n,p);}
s32_t __real_spiffs_api_erase(spiffs *f,uint32_t a,uint32_t n){erases++;return hal(f,a,n,NULL);}
static s32_t call(unsigned op,uint32_t size){
 static uint8_t data[4];errno=EACCES;
 s32_t result=op==0?__wrap_spiffs_api_read(&fs,123,size,data):op==1?__wrap_spiffs_api_write(&fs,123,size,data):__wrap_spiffs_api_erase(&fs,123,size);
 assert(result==hal_result&&errno==EIO&&observed_fs==&fs&&observed_address==123&&observed_size==size);
 assert(observed_buffer==(op==2?NULL:data));return result;
}
static void ready(void){
 assert(!pet_storage_io_ready()&&!pet_storage_io_begin(1000));
 assert(pet_storage_io_register(PET_STORAGE_IO_MAIN,(void*)1));
 assert(!pet_storage_io_register(PET_STORAGE_IO_SAVE,(void*)1));
 assert(pet_storage_io_register(PET_STORAGE_IO_SAVE,(void*)2));
 assert(pet_storage_io_register(PET_STORAGE_IO_PROTOCOL,(void*)3));assert(pet_storage_io_ready());
}
int main(void){
 pet_storage_io_status_t latest={.generation=42};assert(!pet_storage_io_latest(&latest)&&latest.generation==42);
 call(0,16);assert(reads==1&&io_status.generation==0);ready();
 assert(!pet_storage_io_begin(0)&&!pet_storage_io_begin(PET_STORAGE_IO_MAX_WINDOW_US+1));
 uint32_t g=pet_storage_io_begin(1000);assert(g==1&&!pet_storage_io_register(PET_STORAGE_IO_MAIN,(void*)8));
 assert(pet_storage_io_latest(&latest)&&latest.generation==g&&latest.active);
 assert(!pet_storage_io_latest(&io_status));
 task_id=3;call(0,16);call(0,256);hal_result=-23;call(1,4096);task_id=4;call(2,4097);hal_result=0;
 assert(reads==3&&writes==1&&erases==1&&!io_meta_depth);
 pet_storage_io_counter_t out={.calls=77};assert(!pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_READ,&out)&&out.calls==77);
 assert(pet_storage_io_stop(g));assert(pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_READ,&out));
 assert(out.calls==2&&!out.errors&&out.bytes==272&&out.wall_us==34&&out.max_wall_us==17&&out.buckets[0]==1&&out.buckets[1]==1);
 assert(pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_WRITE,&out)&&out.calls==1&&out.errors==1&&out.bytes==4096&&out.buckets[2]==1);
 assert(pet_storage_io_read(g,PET_STORAGE_IO_OTHER,PET_STORAGE_IO_ERASE,&out)&&out.calls==1&&out.errors==1&&out.buckets[3]==1);
 out.calls=77;assert(!pet_storage_io_read(g,0,0,&out)&&out.calls==77);assert(!pet_storage_io_read(g,PET_STORAGE_IO_MAIN,(pet_storage_io_operation_t)-1,&out)&&out.calls==77);
 assert(!pet_storage_io_stop(g+1));pet_storage_io_status_t status;assert(pet_storage_io_status(g,&status)&&status.complete&&!status.flags);
  assert(!pet_storage_io_status(g,&io_status));assert(!pet_storage_io_read(g,PET_STORAGE_IO_MAIN,PET_STORAGE_IO_READ,&counters[0][0]));
 g=pet_storage_io_begin(1000);assert(g==2);task_id=3;stop_in_hal=true;call(0,32);stop_in_hal=false;
 assert(pet_storage_io_status(g,&status)&&status.complete&&(status.flags&PET_STORAGE_IO_LATE_RETURN));
 assert(pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_READ,&out)&&out.calls==1);
 g=pet_storage_io_begin(1);clock_us+=2;assert(pet_storage_io_status(g,&status)&&status.complete&&(status.flags&PET_STORAGE_IO_EXPIRED));
 unsigned old=reads;call(0,1);assert(reads==old+1);assert(pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_READ,&out)&&!out.calls);
 g=pet_storage_io_begin(1000);clock_us--;assert(pet_storage_io_status(g,&status)&&status.complete&&(status.flags&PET_STORAGE_IO_BAD_CLOCK));
 clock_us+=10;g=pet_storage_io_begin(1000);assert(g);counters[2][0]=(pet_storage_io_counter_t){.bytes=UINT64_MAX,.wall_us=UINT64_MAX,.calls=UINT32_MAX,.buckets={UINT32_MAX}};
 call(0,16);assert(pet_storage_io_stop(g));assert(pet_storage_io_read(g,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_READ,&out));
 assert(out.bytes==UINT64_MAX&&out.wall_us==UINT64_MAX&&out.calls==UINT32_MAX&&out.buckets[0]==UINT32_MAX);
 assert(pet_storage_io_status(g,&status)&&(status.flags&PET_STORAGE_IO_OVERFLOW));
 pet_storage_io_status_t saved=io_status;clock_us=-1;assert(!pet_storage_io_begin(1000)&&!memcmp(&saved,&io_status,sizeof(saved)));clock_us=10000;
 io_status.generation=UINT32_MAX;assert(!pet_storage_io_begin(1000));call(0,1);assert(io_status.generation==UINT32_MAX&&!io_meta_depth);
 puts("PASS exact HAL once/args/errno, bounded windows, actors, in-flight retirement, expiry/clock/exhaustion, no cache");return 0;
}
