/* Exact 512-byte-page metadata. No C bitfields, address truncation or IO.
 * Only the existing private cache uses this representation. All 32-bit aligned
 * addresses retain their 23 page bits; pins/fill survive generation invalidation.
 * Access is serialized by io_mux, not by atomics on packed sub-fields. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { PET_PAGE_PIN_MAX=255 };
#define PET_PAGE_ADDRESS_MASK UINT32_C(0x007fffff)
#define PET_PAGE_PIN_MASK UINT32_C(0x7f800000)
#define PET_PAGE_FILL_MASK UINT32_C(0x80000000)
typedef struct { uint32_t packed,generation; } page_cache_entry_t;
_Static_assert(sizeof(page_cache_entry_t)==8,"bounded exact-page metadata");
static inline uint32_t page_entry_address(const page_cache_entry_t *e){return (e->packed&PET_PAGE_ADDRESS_MASK)<<9;}
static inline unsigned page_entry_pins(const page_cache_entry_t *e){return (e->packed&PET_PAGE_PIN_MASK)>>23;}
static inline bool page_entry_filling(const page_cache_entry_t *e){return (e->packed&PET_PAGE_FILL_MASK)!=0;}
static inline bool page_entry_pin(page_cache_entry_t *e){
  if(page_entry_pins(e)==PET_PAGE_PIN_MAX)return false;
  e->packed+=UINT32_C(1)<<23;return true;
}
static inline bool page_entry_unpin(page_cache_entry_t *e){
  if(!page_entry_pins(e))return false;
  e->packed-=UINT32_C(1)<<23;return true;
}
static inline page_cache_entry_t page_entry_reserved(uint32_t address){
  return (page_cache_entry_t){.packed=(address>>9)|(UINT32_C(1)<<23)|PET_PAGE_FILL_MASK};
}
static inline void page_entry_complete(page_cache_entry_t *e){e->packed&=PET_PAGE_ADDRESS_MASK;}
