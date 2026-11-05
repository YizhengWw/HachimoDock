"""Completed numeric windows cannot be silently replaced by another actor."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_retained_completed_windows_preserve_actor_generation_and_exact_counters(tmp_path):
    source=tmp_path/'completed.c';binary=tmp_path/'completed'
    source.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
unsigned io_meta_depth;
#include "pet_p4_storage_io_trace.c"
static int64_t now=100;static void* task=(void*)3;static spiffs fs;
static bool late;static uint32_t late_generation;
int64_t esp_timer_get_time(void){assert(!io_meta_depth);return now;}
void*xTaskGetCurrentTaskHandle(void){assert(!io_meta_depth);return task;}
static s32_t access_hal(uint32_t bytes){
 assert(!io_meta_depth);now+=bytes;
 if(late){late_generation=io_status.generation;assert(pet_storage_io_stop(late_generation));
   assert(!io_status.complete&&io_status.inflight==1);}
 return 0;
}
s32_t __real_spiffs_api_read(spiffs*f,uint32_t a,uint32_t n,uint8_t*p){(void)f;(void)a;(void)p;return access_hal(n);}
s32_t __real_spiffs_api_write(spiffs*f,uint32_t a,uint32_t n,uint8_t*p){(void)f;(void)a;(void)p;return access_hal(n);}
s32_t __real_spiffs_api_erase(spiffs*f,uint32_t a,uint32_t n){(void)f;(void)a;return access_hal(n);}
int main(void){
 for(unsigned i=1;i<=3;i++)assert(pet_storage_io_register(i,(void*)(uintptr_t)i));
 pet_storage_io_status_t s={.generation=77};pet_storage_io_counter_t out[3]={{.calls=88}};
 assert(!pet_storage_io_completed_read(3,0,3,&s,out)&&s.generation==77&&out[0].calls==88);
 uint32_t protocol=pet_storage_io_begin(1000);assert(protocol&&io_status.owner_actor==3);
 uint8_t data[8];assert(!__wrap_spiffs_api_read(&fs,0,8,data));assert(!__wrap_spiffs_api_write(&fs,0,4,data));
 assert(!pet_storage_io_completed_read(3,protocol,3,&s,out));assert(pet_storage_io_stop(protocol));
 assert(pet_storage_io_completed_read(3,protocol,3,&s,out)&&s.generation==protocol&&s.complete&&!s.flags);
 assert(out[0].calls==1&&out[0].bytes==8&&out[1].calls==1&&out[1].bytes==4&&!out[2].calls);
 task=(void*)2;uint32_t save=pet_storage_io_begin(1000);assert(save>protocol);
 assert(!__wrap_spiffs_api_read(&fs,0,2,data));assert(pet_storage_io_stop(save));
 assert(pet_storage_io_completed_read(3,0,3,&s,out)&&s.generation==protocol&&out[0].bytes==8);
 assert(pet_storage_io_completed_read(2,save,2,&s,out)&&s.generation==save&&out[0].bytes==2);
 task=(void*)3;uint32_t next=pet_storage_io_begin(1000);assert(next>save);
 assert(pet_storage_io_completed_read(3,protocol,3,&s,out)&&s.generation==protocol);
 late=true;assert(!__wrap_spiffs_api_read(&fs,0,1,data));late=false;
 assert(late_generation==next&&pet_storage_io_completed_read(3,next,3,&s,out));
 assert(out[0].calls==1&&out[0].bytes==1&&(s.flags&PET_STORAGE_IO_LATE_RETURN));
 s.generation=77;out[0].calls=88;
 assert(!pet_storage_io_completed_read(3,protocol,3,&s,out)&&s.generation==77&&out[0].calls==88);
 assert(!pet_storage_io_completed_read(0,0,3,&s,out));assert(!pet_storage_io_completed_read(4,0,3,&s,out));
 assert(!pet_storage_io_completed_read(3,0,0,&s,out));assert(!pet_storage_io_completed_read(3,0,5,&s,out));
 assert(!pet_storage_io_completed_read(3,0,3,&io_status,out));
 assert(!pet_storage_io_completed_read(3,0,3,&s,completed_io[0].counters[0]));
 union {pet_storage_io_status_t status;pet_storage_io_counter_t counters[3];} alias;
 assert(!pet_storage_io_completed_read(3,0,3,&alias.status,alias.counters));
 assert(sizeof(completed_io)<=2048&&!io_meta_depth);return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
        '-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_P4_STORAGE_IO_RETAIN_COMPLETED=1','-DPET_STORAGE_IO_HOST_TEST=1',
        '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/storage_io_stubs'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
