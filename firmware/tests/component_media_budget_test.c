
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned stat_calls,unlink_calls;
static int missing_stat(const char *path,struct stat *out){(void)path;(void)out;stat_calls++;errno=ENOENT;return -1;}
static int missing_unlink(const char *path){(void)path;unlink_calls++;errno=ENOENT;return -1;}
#define stat(path,out) missing_stat(path,out)
#define unlink missing_unlink
#include "pet_p4_component_media.c"
#undef stat
#undef unlink
int main(void){
 pet_cext_runtime_t empty={0};
 assert(pet_cmedia_persist(3,0,&empty));
 printf("ordinary component: stat=%u cleanup=%u\n",stat_calls,unlink_calls);
 assert(stat_calls==0);
 assert(unlink_calls==2*(PET_SAMPLE_MAX_SLOTS+PET_CEXT_CLIP_MAX+PET_CEXT_BIG_MAX));
 /* Outstanding publication still rejects even a zero-media install. */
 g_pending.serial=1;unsigned prior=unlink_calls;
 assert(!pet_cmedia_persist(3,0,&empty)&&unlink_calls==prior);g_pending.serial=0;
 assert(!pet_cmedia_persist(PET_CEV_COMPONENTS,0,&empty));
 assert(!pet_cmedia_persist(3,2,&empty));
 /* A real sample must retain the existing full on-disk budget check. */
 uint8_t sample[]={'P','4','A','1',1,0,0,0,0,0};
 assert(pet_cmedia_stage_chunk(PET_CEXT_MEDIA_SAMPLE,"tone",0,sample,sizeof(sample),NULL,0));
 empty.sample_count=1;strcpy(empty.sample[0].id,"tone");
 stat_calls=0;
 assert(disk_budget_ok()&&stat_calls==896);
 g_stage.used=PET_CMEDIA_DEVICE_BUDGET_BYTES+1u;stat_calls=0;
 assert(!disk_budget_ok()&&!stat_calls);g_stage.used=sizeof(sample);
 pet_cmedia_stage_reset();return 0;
}
