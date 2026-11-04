/* Included once by private HAL observer. Exact successful byte snapshots,
 * never filename/existence authority. No read-ahead or physical IO here.
 * Native mount-once parent + <=5s epoch. Hits cannot detect a new media fault.
 * External/PSRAM copies are interruptible, never under the metadata lock. */
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
#include "pet_p4_storage_sparse_page.h"
static unsigned page_format_bit(unsigned way){return 2U<<way;}
#endif
static unsigned page_set(uint32_t address){
#if PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH
  /* Numeric key distribution only, NOT a lookup-page/existence authority.
   * Keep the 4KiB-strided working set in two ways without random hot-set
   * collisions; other pages remain exact-address keys and may evict normally.
   * Correctness does not depend on geometry; only hit rate can change. */
  return ((address>>12)+((address>>9)&7U)*257U)&(PAGE_CACHE_SETS-1);
#else
  uint32_t x=address>>9;x^=x>>11;x*=UINT32_C(0x9e3779b1);x^=x>>16;return x&(PAGE_CACHE_SETS-1);
#endif
}
static bool page_eligible(io_ticket_t t,spiffs *fs,uint32_t address,uint32_t size,const void *buffer){
  return t.generation&&!t.physical&&fs&&buffer&&size==PAGE_CACHE_BYTES&&(address&(PAGE_CACHE_BYTES-1))==0;
}
static bool page_current_locked(io_ticket_t t,spiffs *fs){
  if(!io_status.active||io_status.generation!=t.generation||page_disabled_generation==t.generation||page_mutation==UINT64_MAX)return false;
  if(page_owner_generation!=t.generation){page_owner_generation=t.generation;page_owner=fs;}
  return page_owner==fs;
}
static bool page_read(io_ticket_t t,spiffs *fs,uint32_t address,uint32_t size,uint8_t *buffer){
#if PET_P4_STORAGE_PAGE_CACHE_PARTIAL
  /* Reuse ONLY bytes from an already successful full-page snapshot. Never
   * fetch a bigger physical range, cache existence, or fill from a partial. */
  uint32_t offset=address&(PAGE_CACHE_BYTES-1);
  if(!t.generation||t.physical||!fs||!buffer||!size||size>PAGE_CACHE_BYTES-offset)return false;
  address-=offset;
#else
  uint32_t offset=0;
  if(!page_eligible(t,fs,address,size,buffer))return false;
#endif
  unsigned set=page_set(address),slot=PAGE_CACHE_SLOTS;
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
  bool sparse=false;
#endif
  int64_t now=esp_timer_get_time();portENTER_CRITICAL(&io_mux);expire_locked(now);
  if(page_current_locked(t,fs)){
    for(unsigned way=0;way<PAGE_CACHE_WAYS;way++){
      page_cache_entry_t *e=&page_entries[set][way];
      if(e->generation==t.generation&&page_entry_address(e)==address&&!page_entry_filling(e)&&page_entry_pins(e)<PET_PAGE_PIN_MAX){
#if PET_P4_STORAGE_PAGE_CACHE_APPLY
        if(page_entry_pin(e)){
          slot=set*PAGE_CACHE_WAYS+way;
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
          sparse=(page_replace[set]&page_format_bit(way))!=0;
#endif
        }
#endif
        break;
      }
    }
  }
  portEXIT_CRITICAL(&io_mux);if(slot==PAGE_CACHE_SLOTS)return false;
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
  if(sparse)pet_sparse_page_copy(buffer,page_bytes[slot],offset,size);
  else
#endif
  memcpy(buffer,page_bytes[slot]+offset,size);
  portENTER_CRITICAL(&io_mux);
  page_cache_entry_t *e=&page_entries[set][slot%PAGE_CACHE_WAYS];
  if(!page_entry_unpin(e))io_status.flags|=PET_STORAGE_IO_OVERFLOW;
  portEXIT_CRITICAL(&io_mux);return true;
}
static void page_fill(io_ticket_t t,spiffs *fs,uint32_t address,uint32_t size,const uint8_t *buffer){
  if(!page_eligible(t,fs,address,size,buffer))return;
  uint8_t copy[PAGE_CACHE_BYTES];memcpy(copy,buffer,sizeof(copy));
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
  bool sparse=pet_sparse_page_eligible(copy);
#endif
  unsigned set=page_set(address),way=PAGE_CACHE_WAYS;
  int64_t now=esp_timer_get_time();portENTER_CRITICAL(&io_mux);expire_locked(now);
  if(page_current_locked(t,fs)&&page_mutation==t.mutation){
    for(unsigned w=0;w<PAGE_CACHE_WAYS;w++){
      page_cache_entry_t *e=&page_entries[set][w];
      if(!page_entry_pins(e)&&!page_entry_filling(e)&&(e->generation!=t.generation||page_entry_address(e)==address)){way=w;break;}
    }
    if(way==PAGE_CACHE_WAYS){
      for(unsigned w=0;w<PAGE_CACHE_WAYS;w++){
        unsigned candidate=(page_replace[set]+w)%PAGE_CACHE_WAYS;
        page_cache_entry_t *e=&page_entries[set][candidate];
        if(!page_entry_pins(e)&&!page_entry_filling(e)){
          way=candidate;
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
          /* Cursor and BOTH format bits share the old byte; never clear a
           * different way's live format while selecting this victim. */
          page_replace[set]=(uint8_t)((page_replace[set]&~1U)|((candidate+1)%PAGE_CACHE_WAYS));
#else
          page_replace[set]=(uint8_t)((candidate+1)%PAGE_CACHE_WAYS);
#endif
          break;
        }
      }
    }
    if(way<PAGE_CACHE_WAYS)page_entries[set][way]=page_entry_reserved(address);
  }
  portEXIT_CRITICAL(&io_mux);if(way==PAGE_CACHE_WAYS)return;
  unsigned slot=set*PAGE_CACHE_WAYS+way;
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
  if(sparse)pet_sparse_page_store(page_bytes[slot],copy);
  else
#endif
  memcpy(page_bytes[slot],copy,sizeof(copy));
  now=esp_timer_get_time();portENTER_CRITICAL(&io_mux);expire_locked(now);
  page_cache_entry_t *e=&page_entries[set][way];
  if(page_current_locked(t,fs)&&page_mutation==t.mutation){
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE
    unsigned bit=page_format_bit(way);
    page_replace[set]=(uint8_t)((page_replace[set]&~bit)|(sparse?bit:0));
#endif
    e->generation=t.generation;
  }
  /* No invalidator resets pins/filling; no allocator can reuse this reserved
   * fixed slot until the interruptible copy physically returned. */
  page_entry_complete(e);portEXIT_CRITICAL(&io_mux);
}
static void page_invalidate(spiffs *fs,uint32_t address,uint32_t size){
  uint64_t end=(uint64_t)address+size,first=(uint64_t)address&~UINT64_C(511);
  uint64_t count=size?(end-first+511)/512:0;
  portENTER_CRITICAL(&io_mux);
  if(page_mutation<UINT64_MAX)page_mutation++;
  if(end>UINT64_C(0x100000000)||count>16||page_mutation==UINT64_MAX)page_disabled_generation=io_status.generation;
  else if(page_owner==fs&&page_owner_generation==io_status.generation){
    for(unsigned i=0;i<count;i++){
      uint32_t address_page=(uint32_t)(first+(uint64_t)i*512);unsigned set=page_set(address_page);
      for(unsigned w=0;w<PAGE_CACHE_WAYS;w++){
        page_cache_entry_t *e=&page_entries[set][w];if(page_entry_address(e)==address_page)e->generation=0;
      }
    }
  }
  portEXIT_CRITICAL(&io_mux);
}
