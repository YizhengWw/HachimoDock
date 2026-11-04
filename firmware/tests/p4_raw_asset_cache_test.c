#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 2
typedef struct { uint32_t address, size; unsigned slot; } esp_partition_t;
static const esp_partition_t partitions[2] = {{0,65536,0},{65536,65536,1}};
static uint8_t flash_bytes[2][65536];
static pthread_mutex_t flash_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cond = PTHREAD_COND_INITIALIZER;
static bool read_entered, read_resume;
static _Atomic bool block_read, fail_read, fail_alloc;
static _Atomic bool block_erase, block_write, fail_erase, fail_write, fail_malloc;
static pthread_mutex_t mutation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mutation_cond = PTHREAD_COND_INITIALIZER;
static bool mutation_entered, mutation_resume;
static _Atomic unsigned allocations, frees, reads;
static _Thread_local unsigned critical_depth;
#define portMUX_TYPE pthread_mutex_t
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) do { assert(!critical_depth); pthread_mutex_lock(m); critical_depth++; } while (0)
#define portEXIT_CRITICAL(m) do { critical_depth--; pthread_mutex_unlock(m); } while (0)
#define pdMS_TO_TICKS(n) (n)
static void vTaskDelay(unsigned ticks) { assert(!critical_depth && ticks == 2); }
static const esp_partition_t *esp_partition_find_first(unsigned type, unsigned sub, const char *label) {
  assert(!critical_depth && type == 1 && sub == 2);
  return !strcmp(label,"appearance0") ? &partitions[0] : !strcmp(label,"appearance1") ? &partitions[1] : NULL;
}
static int esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t size) {
  assert(!critical_depth && p && out && offset + size <= p->size); reads++;
  if (fail_read) return 1;
  pthread_mutex_lock(&flash_lock); memcpy(out, flash_bytes[p->slot] + offset, size); pthread_mutex_unlock(&flash_lock);
  if (block_read) {
    pthread_mutex_lock(&read_lock); read_entered = true; pthread_cond_broadcast(&read_cond);
    while (!read_resume) pthread_cond_wait(&read_cond, &read_lock);
    pthread_mutex_unlock(&read_lock);
  }
  return ESP_OK;
}
static void mutation_barrier(bool blocked) {
  assert(!critical_depth);
  if (!blocked) return;
  pthread_mutex_lock(&mutation_lock); mutation_entered = true; pthread_cond_broadcast(&mutation_cond);
  while (!mutation_resume) pthread_cond_wait(&mutation_cond, &mutation_lock);
  pthread_mutex_unlock(&mutation_lock);
}
static int esp_partition_write(const esp_partition_t *p, size_t offset, const void *in, size_t size) {
  assert(!critical_depth && p && offset + size <= p->size);
  mutation_barrier(block_write);
  if (fail_write) return 1;
  pthread_mutex_lock(&flash_lock); memcpy(flash_bytes[p->slot] + offset, in, size); pthread_mutex_unlock(&flash_lock); return ESP_OK;
}
static int esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size) {
  assert(!critical_depth && p && offset + size <= p->size);
  mutation_barrier(block_erase);
  if (fail_erase) return 1;
  pthread_mutex_lock(&flash_lock); memset(flash_bytes[p->slot] + offset, 255, size); pthread_mutex_unlock(&flash_lock); return ESP_OK;
}
static void *tracked_calloc(size_t n, size_t size) {
  assert(!critical_depth && n * size <= 5120);
  if (fail_alloc) return NULL;
  allocations++; return calloc(n, size);
}
static void *tracked_malloc(size_t size) {
  assert(!critical_depth && size == 4096);
  if (fail_malloc) return NULL;
  allocations++; return malloc(size);
}
static void tracked_free(void *p) { assert(!critical_depth); if (p) frees++; free(p); }
#define calloc tracked_calloc
#define malloc tracked_malloc
#define free tracked_free
/* RAW_PRODUCTION */
#undef calloc
#undef malloc
#undef free

static void header(unsigned slot, uint32_t offset, char pack) {
  pthread_mutex_lock(&flash_lock);
  uint8_t *bytes = flash_bytes[slot]; memset(bytes, 0, 4096);
  memcpy(bytes, PET_P4_RAW_MAGIC, 8); put_u32(bytes+8, 1); put_u32(bytes+12, 1); put_u32(bytes+16, offset+64);
  memset(bytes+24, pack, 64);
  put_u64(bytes+88, path_hash("p4/a.h264")); put_u32(bytes+96, offset); put_u32(bytes+100, 64);
  memset(bytes+offset, 0x45, 64);
  pthread_mutex_unlock(&flash_lock);
}
static void *old_loader(void *unused) {
  (void)unused; uint32_t offset=123;
  assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset)); assert(offset==123); return NULL;
}
static void invalidate_cache_for_test(int slot) {
  assert(begin_cache_mutation(slot)); end_cache_mutation(slot);
}
static const char new_pack[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static void stage_commit(void) {
  char error[128]; uint8_t data[64] = {0x45};
  assert(pet_p4_raw_assets_prepare(8192,error,sizeof(error)));
  assert(pet_p4_raw_assets_write_chunk("p4/a.h264","0",data,sizeof(data),error,sizeof(error)));
  assert(pet_p4_raw_assets_finish_file("p4/a.h264",sizeof(data),NULL,error,sizeof(error)));
}
static void *mutate_header(void *arg) {
  char error[128]; int op = (int)(intptr_t)arg;
  bool result = op == 0 ? pet_p4_raw_assets_invalidate(error,sizeof(error))
    : op == 1 ? pet_p4_raw_assets_prepare(8192,error,sizeof(error))
    : pet_p4_raw_assets_commit(new_pack,error,sizeof(error));
  assert(result == !(op == 2 ? fail_write : fail_erase));
  return NULL;
}
static void wait_mutation(void) {
  pthread_mutex_lock(&mutation_lock);
  while (!mutation_entered) pthread_cond_wait(&mutation_cond,&mutation_lock);
  pthread_mutex_unlock(&mutation_lock);
}
static void resume_mutation(void) {
  pthread_mutex_lock(&mutation_lock); mutation_resume=true; pthread_cond_broadcast(&mutation_cond);
  pthread_mutex_unlock(&mutation_lock);
}
static void assert_no_mutation(void) {
  assert(!g_cache_mutating[1] && !g_caches[1].attempted && !g_caches[1].valid);
  assert(allocations == frees);
}
int main(void) {
  header(1,4096,'a'); uint32_t offset=0;
  assert(pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset) && offset==4096);
  assert(allocations==frees && allocations==1);
  char pack[65]; assert(pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack)) && pack[0]=='a' && !pack[64]);
  assert(!pet_p4_raw_assets_locate(-1,"p4/a.h264",64,&offset));
  assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",63,&offset));
  invalidate_cache_for_test(1); block_read=true;
  pthread_t thread; assert(!pthread_create(&thread,NULL,old_loader,NULL));
  pthread_mutex_lock(&read_lock); while(!read_entered) pthread_cond_wait(&read_cond,&read_lock); pthread_mutex_unlock(&read_lock);
  /* Invalidation cannot free the in-flight parse buffer or admit a second one. */
  invalidate_cache_for_test(1); header(1,8192,'b'); unsigned held=allocations;
  assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset)); assert(allocations==held && allocations==frees+1);
  pthread_mutex_lock(&read_lock); read_resume=true; pthread_cond_broadcast(&read_cond); pthread_mutex_unlock(&read_lock);
  assert(!pthread_join(thread,NULL)); block_read=false;
  assert(allocations==frees && !g_caches[1].valid && !g_caches[1].attempted && !g_cache_loading[1]);
  assert(pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset) && offset==8192);
  assert(pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack)) && pack[0]=='b');
  uint8_t out[64]; assert(pet_p4_raw_assets_read_all_from_slot(1,"p4/a.h264",out,sizeof(out)));
  for(unsigned i=0;i<64;i++) assert(out[i]==0x45);
  unsigned before=reads;
  assert(!pet_p4_raw_assets_read_range(1,8192,64,0,out,16385));
  assert(!pet_p4_raw_assets_read_range(1,8192,64,SIZE_MAX,out,1));
  assert(!pet_p4_raw_assets_read_range(1,8192,SIZE_MAX,0,out,1));
  assert(!pet_p4_raw_assets_read_range(1,4095,64,0,out,1));
  assert(reads==before);
  assert(pet_p4_raw_assets_read_range(1,8192,64,32,out,32));
  invalidate_cache_for_test(1); fail_read=true;
  assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset)); fail_read=false;
  assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset)); /* Bounded negative cache. */
  invalidate_cache_for_test(1); fail_alloc=true;
  assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset)); fail_alloc=false;
  assert(!g_cache_loading[1] && !g_caches[1].attempted);
  assert(pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset));
  header(0,4096,'c'); assert(pet_p4_raw_assets_locate(0,"p4/a.h264",64,&offset) && offset==4096);
  assert(pet_p4_raw_assets_committed_pack_id_for_slot(0,pack,sizeof(pack)) && pack[0]=='c');
  /* Mutation first: the real erase/write is paused BEFORE changing flash.
     Readers must not read/publish the still-valid old header in the new epoch.
     A second writer is rejected and must not clear the first writer's latch. */
  for (int op=0;op<3;op++) for (int failure=0;failure<2;failure++) {
    if(op==2) stage_commit();
    header(1,4096,'a'); invalidate_cache_for_test(1);
    assert(pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack)) && pack[0]=='a');
    block_erase=op!=2; block_write=op==2;
    fail_erase=failure && op!=2; fail_write=failure && op==2;
    mutation_entered=mutation_resume=false;
    assert(!pthread_create(&thread,NULL,mutate_header,(void *)(intptr_t)op)); wait_mutation();
    unsigned held_reads=reads, held_allocations=allocations;
    uint64_t held_epoch=g_cache_epochs[1]; char error[128]; offset=123;
    bool held_prepared=g_transfer.prepared;
    unsigned held_entry_count=g_transfer.entry_count;
    assert(g_cache_mutating[1]);
    assert(!pet_p4_raw_assets_locate(1,"p4/a.h264",64,&offset) && offset==123);
    assert(!pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack)));
    assert(!g_cache_loading[1] && !g_caches[1].attempted);
    assert(reads==held_reads && allocations==held_allocations);
    assert(!pet_p4_raw_assets_invalidate(error,sizeof(error)));
    assert(!pet_p4_raw_assets_prepare(8192,error,sizeof(error)));
    assert(!pet_p4_raw_assets_commit(new_pack,error,sizeof(error)));
    assert(g_cache_mutating[1] && g_cache_epochs[1]==held_epoch);
    assert(g_transfer.prepared==held_prepared && g_transfer.entry_count==held_entry_count);
    assert(reads==held_reads && allocations==held_allocations);
    /* Another immutable slot remains readable while slot 1 is mutated. */
    assert(pet_p4_raw_assets_committed_pack_id_for_slot(0,pack,sizeof(pack)) && pack[0]=='c');
    resume_mutation(); assert(!pthread_join(thread,NULL));
    block_erase=block_write=fail_erase=fail_write=false;
    assert_no_mutation();
    assert(g_cache_epochs[1]==held_epoch+1);
    bool has_pack=pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack));
    assert(reads==held_reads+1); /* No stale positive OR negative cache survives. */
    assert(has_pack==(failure || op==2));
    if(has_pack) assert(pack[0]==(!failure && op==2?'b':'a'));
  }
  /* Reader first: publish after a complete real erase or commit must fail. */
  for (int op=0;op<3;op++) for (int failure=0;failure<2;failure++) {
    if(op==2) stage_commit();
    header(1,4096,'a'); invalidate_cache_for_test(1);
    block_read=true; read_entered=read_resume=false;
    assert(!pthread_create(&thread,NULL,old_loader,NULL));
    pthread_mutex_lock(&read_lock); while(!read_entered) pthread_cond_wait(&read_cond,&read_lock); pthread_mutex_unlock(&read_lock);
    fail_erase=failure && op!=2; fail_write=failure && op==2;
    mutate_header((void *)(intptr_t)op);
    fail_erase=fail_write=false;
    assert(!g_cache_mutating[1] && g_cache_loading[1]);
    pthread_mutex_lock(&read_lock); read_resume=true; pthread_cond_broadcast(&read_cond); pthread_mutex_unlock(&read_lock);
    assert(!pthread_join(thread,NULL)); block_read=false;
    assert_no_mutation(); assert(!g_cache_loading[1]);
    bool has_pack=pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack));
    assert(has_pack==(failure || op==2));
    if(has_pack) assert(pack[0]==(!failure && op==2?'b':'a'));
  }
  /* Pre-I/O commit failures release only their own admission and invalidate. */
  char error[128]; stage_commit(); fail_malloc=true;
  assert(!pet_p4_raw_assets_commit(new_pack,error,sizeof(error))); fail_malloc=false;
  assert_no_mutation();
  assert(!pet_p4_raw_assets_commit("invalid",error,sizeof(error))); assert_no_mutation();
  assert(pet_p4_raw_assets_commit(new_pack,error,sizeof(error))); assert_no_mutation();
  assert(allocations==frees && sizeof(pet_p4_raw_cache_t)<=1024);
  puts("raw cache production epoch/IO publication tests passed");
  return 0;
}
