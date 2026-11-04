/* Exact native512-byte snapshot representation, no IO or authority. The same
 * fixed512-byte slot owns either all bytes or exact32+uniform448+exact32. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static inline bool pet_sparse_page_eligible(const uint8_t p[512]){
  for(unsigned i=33;i<480;i++)if(p[i]!=p[32])return false;
  return true;
}
static inline void pet_sparse_page_store(uint8_t slot[512],const uint8_t p[512]){
  memcpy(slot,p,32);slot[32]=p[32];memcpy(slot+33,p+480,32);
}
/* Caller validates a contained nonempty slice before acquiring its slot pin.
 * The source is immutable while pinned, including during nested invalidation. */
static inline void pet_sparse_page_copy(uint8_t *out,const uint8_t slot[512],unsigned offset,unsigned size){
  if(offset<32){unsigned n=32-offset;if(n>size)n=size;memcpy(out,slot+offset,n);out+=n;offset+=n;size-=n;}
  if(size&&offset<480){unsigned n=480-offset;if(n>size)n=size;memset(out,slot[32],n);out+=n;offset+=n;size-=n;}
  if(size)memcpy(out,slot+33+offset-480,size);
}
