/* Included once in the private HAL observer. Successful constant-prefix byte ranges,
 * not file absence. A hit deliberately does not detect a new media fault;
 * original miss errors are unchanged. Max lifetime is the existing <=5s trace
 * window. Private parent mounts once before observer registration; no remount
 * or media replacement may occur inside a window. No read ahead or IO here.
 */
static unsigned uniform_set(uint32_t address){
  uint32_t x=address>>9;x^=x>>11;x*=UINT32_C(0x9e3779b1);x^=x>>16;
  return x&(UNIFORM_SETS-1);
}
static bool uniform_eligible(io_ticket_t t,spiffs *fs,uint32_t address,uint32_t size){
  return t.generation&&!t.physical&&fs&&size==UNIFORM_BYTES&&(address&(UNIFORM_BYTES-1))==0;
}
static bool uniform_current_locked(io_ticket_t t){
  return io_status.active&&io_status.generation==t.generation
    &&uniform_disabled_generation!=t.generation&&uniform_mutation<UINT64_MAX;
}
static bool uniform_owner_locked(io_ticket_t t,spiffs *fs){
  if(!uniform_current_locked(t))return false;
  /* Exactly one opaque mount identity per window. A different filesystem
   * always misses; no per-entry pointer overhead or identity truncation. */
  if(uniform_owner_generation!=t.generation){uniform_owner_generation=t.generation;uniform_owner=fs;}
  return uniform_owner==fs;
}
static bool uniform_get(io_ticket_t t,spiffs *fs,uint32_t address,uint32_t size,uniform_bytes_t *value){
  if(!uniform_eligible(t,fs,address,size))return false;
  int64_t now=esp_timer_get_time();bool hit=false;portENTER_CRITICAL(&io_mux);expire_locked(now);
  if(uniform_owner_locked(t,fs)){
    for(unsigned way=0;way<UNIFORM_WAYS;way++){
      uniform_entry_t *e=&uniform_entries[uniform_set(address)][way];
      if(e->generation==t.generation&&e->address==address){
        value->value=e->value;memcpy(value->tail,e->tail,4);
#if PET_P4_STORAGE_UNIFORM_CACHE_APPLY
        hit=true;
#endif
        break;
      }
    }
  }
  portEXIT_CRITICAL(&io_mux);return hit;
}
static void uniform_fill(io_ticket_t t,spiffs *fs,uint32_t address,uint32_t size,const uint8_t *buffer){
  if(!uniform_eligible(t,fs,address,size)||!buffer)return;
  /* Full external-buffer inspection is interruptible, never metadata-locked. */
  uint8_t value=buffer[0],tail[4];for(unsigned i=1;i<UNIFORM_BYTES-4;i++)if(buffer[i]!=value)return;
  memcpy(tail,buffer+UNIFORM_BYTES-4,4);
  int64_t now=esp_timer_get_time();unsigned set=uniform_set(address);portENTER_CRITICAL(&io_mux);expire_locked(now);
  if(uniform_owner_locked(t,fs)&&uniform_mutation==t.mutation){
    unsigned slot=UNIFORM_WAYS;
    for(unsigned way=0;way<UNIFORM_WAYS;way++){
      uniform_entry_t *e=&uniform_entries[set][way];
      if(e->generation!=t.generation){slot=way;break;}
      if(e->address==address){slot=way;break;}
    }
    if(slot==UNIFORM_WAYS){slot=uniform_replace[set];uniform_replace[set]=(uint8_t)((slot+1)%UNIFORM_WAYS);}
    uniform_entries[set][slot]=(uniform_entry_t){.address=address,.generation=t.generation,.value=value};
    memcpy(uniform_entries[set][slot].tail,tail,4);
  }
  portEXIT_CRITICAL(&io_mux);
}
static void uniform_invalidate(spiffs *fs,uint32_t address,uint32_t size){
  uint64_t end=(uint64_t)address+size;
  uint64_t first=(uint64_t)address&~UINT64_C(511);
  uint64_t count=size?(end-first+511)/512:0;
  portENTER_CRITICAL(&io_mux);
  /* Every attempted mutation, including failed/partial HAL writes, precedes
   * physical IO. A concurrent miss cannot subsequently insert old bytes. */
  if(uniform_mutation<UINT64_MAX)uniform_mutation++;
  if(end>UINT64_C(0x100000000)||count>16||uniform_mutation==UINT64_MAX){
    uniform_disabled_generation=io_status.generation;
  }else if(uniform_owner==fs&&uniform_owner_generation==io_status.generation){
    for(unsigned i=0;i<count;i++){
      uint32_t page=(uint32_t)(first+(uint64_t)i*512);
      for(unsigned way=0;way<UNIFORM_WAYS;way++){
        uniform_entry_t *e=&uniform_entries[uniform_set(page)][way];
        if(e->address==page)e->generation=0;
      }
    }
  }
  portEXIT_CRITICAL(&io_mux);
}
