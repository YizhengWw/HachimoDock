/* Host-only fixture for extracted PRODUCTION functions; never opens a port. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_transport_audit.h"
#include "pet_p4_control_integrity.h"
#include "pet_p4_uart_control_staging.h"
#define DRAM_ATTR
#define PET_P4_UART_NUM 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0

enum {FULL, ODD_FIRST, ODD_ALWAYS, ODD_THEN_ZERO, ZERO, NEGATIVE,
      OVERSIZED, ODD_THEN_OVERSIZED, DRAIN_ERROR, ZERO_FIRST};
static unsigned mode,calls,drains,delays;
static size_t used,largest;
static unsigned char output[8192];
static bool aligned_control;
static const void *first_driver_pointer;
static int uart_write_bytes(int port, const void *data,size_t size) {
  assert(port==0 && size>0);
  calls++; if(!first_driver_pointer) first_driver_pointer=data;
  if(size>largest) largest=size;
  if(aligned_control) {
    assert(((uintptr_t)data % 64)==0);
    assert(data==first_driver_pointer); /* re-stage; NEVER stage + odd accepted */
    assert(size<=PET_P4_UART_TX_BURST_BYTES);
  }
  if(mode==ZERO || (mode==ZERO_FIRST && calls==1) ||
      (mode==ODD_THEN_ZERO && calls>1)) return 0;
  if(mode==NEGATIVE) return -7;
  if(mode==OVERSIZED || (mode==ODD_THEN_OVERSIZED && calls>1)) return (int)size+1;
  size_t n=size;
  if((mode==ODD_FIRST || mode==ODD_THEN_ZERO || mode==ODD_THEN_OVERSIZED) && calls==1 && n>3) n=3;
  if(mode==ODD_ALWAYS && n>1) n=1;
  assert(used+n<=sizeof(output)); memcpy(output+used,data,n); used+=n;
  return (int)n;
}
static int uart_wait_tx_done(int port,int timeout) {
  assert(port==0 && timeout==100); drains++;
  return mode==DRAIN_ERROR?-9:ESP_OK;
}
#if PET_P4_UART_PACING_ACTIVE
static void vTaskDelay(int ticks) { assert(ticks==1); delays++; }
#endif
#if PET_P4_SERIAL_CONTROL_INTEGRITY
static unsigned usb_calls;
static const void *usb_pointer;
static void usb_queue_all(const char *data,size_t size,pet_transport_write_observation_t *o) {
  usb_calls++; usb_pointer=data;
  assert(size==PET_CONTROL_PREFIX_BYTES || size==1);
  if(o)*o=(pet_transport_write_observation_t){.accepted=size,.last_return=(int)size,
    .drain=PET_TRANSPORT_AUDIT_NOT_OBSERVED};
}
#endif
#if PET_P4_SERIAL_CONTROL_INTEGRITY && PET_P4_UART_PACING_ACTIVE
static size_t g_uart_control_prefix_queued;
_Static_assert(PET_CONTROL_PREFIX_BYTES<PET_P4_UART_TX_BURST_BYTES,"prefix bound");
#endif
#include "production_control_queue.inc"

static void reset(unsigned next_mode,bool control) {
  mode=next_mode; calls=drains=delays=0; used=largest=0;
  aligned_control=control && PET_P4_UART_PACING_ACTIVE;
  first_driver_pointer=NULL;
#if PET_P4_SERIAL_CONTROL_INTEGRITY && PET_P4_UART_PACING_ACTIVE
  g_uart_control_prefix_queued=0;
#endif
#if PET_P4_SERIAL_CONTROL_INTEGRITY
  usb_calls=0; usb_pointer=NULL;
#endif
}
static void full_and_odd_partials(const char *input,size_t size) {
  for(unsigned m=FULL;m<=ODD_ALWAYS;m++) {
    reset(m,true); pet_transport_write_observation_t o;
    uart_write_all(input,size,&o);
    assert(o.accepted==size && used==size && !memcmp(input,output,size));
    assert(!o.invalid_return && o.drain==ESP_OK && o.last_return>0);
    unsigned chunks=(unsigned)((size+PET_P4_UART_TX_BURST_BYTES-1)/PET_P4_UART_TX_BURST_BYTES);
    assert(drains==(PET_P4_UART_PACING_ACTIVE?chunks:1));
    assert(delays==(PET_P4_UART_PACING_ACTIVE?chunks:0));
    if(m==FULL)assert(calls==(PET_P4_UART_PACING_ACTIVE?chunks:1));
    if(m==ODD_FIRST)assert(calls==(PET_P4_UART_PACING_ACTIVE?chunks+1:2));
    if(m==ODD_ALWAYS)assert(calls==size);
#if PET_P4_UART_PACING_ACTIVE
    assert(first_driver_pointer==g_uart_control_stage);
    assert(__alignof__(g_uart_control_stage)>=64 && sizeof(g_uart_control_stage)==256);
#else
    assert(first_driver_pointer==input);
#endif
  }
}
static void terminal_results_never_replay(const char *input,size_t size) {
  for(unsigned m=ODD_THEN_ZERO;m<=DRAIN_ERROR;m++) {
    reset(m,true); pet_transport_write_observation_t o;
    uart_write_all(input,size,&o);
    size_t first=PET_P4_UART_PACING_ACTIVE?PET_P4_UART_TX_BURST_BYTES:size;
    size_t accepted=(m==ODD_THEN_ZERO || m==ODD_THEN_OVERSIZED)?3:
                    (m==DRAIN_ERROR?first:0);
    assert(used==accepted && o.accepted==accepted && !memcmp(input,output,accepted));
    assert(drains==1 && delays==0);
    assert(calls==((m==ODD_THEN_ZERO || m==ODD_THEN_OVERSIZED)?2U:1U));
    assert(o.invalid_return==(m==OVERSIZED || m==ODD_THEN_OVERSIZED));
    assert(o.drain==(m==DRAIN_ERROR?-9:ESP_OK));
    if(m==ZERO || m==ODD_THEN_ZERO)assert(o.last_return==0);
    if(m==NEGATIVE)assert(o.last_return==-7);
  }
}
static void queue_and_empty_paths(const char *input,size_t size) {
  reset(FULL,false); pet_transport_write_observation_t o;
  /* Exact original queue used by no-drain audio/media/logs: source and waits unchanged. */
  uart_queue_all(input,size,&o);
  assert(calls==1 && !drains && !delays && first_driver_pointer==input);
  assert(used==size && !memcmp(input,output,size) && o.accepted==size);
  assert(o.drain==PET_TRANSPORT_AUDIT_NOT_OBSERVED);
  reset(FULL,true); uart_write_all(input,0,&o);
  assert(!calls && !used && !delays && !o.accepted && !o.invalid_return);
  assert(drains==(PET_P4_UART_PACING_ACTIVE?0U:1U));
#if PET_P4_UART_PACING_ACTIVE
  reset(FULL,true); uart_control_queue_all(input,257,&o);
  assert(!calls && !used && !drains && !delays && o.invalid_return && !o.accepted);
#endif
}
#if PET_P4_SERIAL_CONTROL_INTEGRITY
static void prefix_debt_and_routes(const char *input,size_t size) {
  char prefix[PET_CONTROL_PREFIX_BYTES+1];size_t bytes;
  assert(pet_control_prefix(prefix,input,&bytes) && bytes==size);
  reset(FULL,true);
  assert(transport_control_prefix(prefix,false));
  assert(calls==1 && !drains && !delays && used==PET_CONTROL_PREFIX_BYTES);
#if PET_P4_UART_PACING_ACTIVE
  assert(g_uart_control_prefix_queued==PET_CONTROL_PREFIX_BYTES);
#endif
  pet_transport_write_observation_t o;uart_write_all(input,size,&o);
  assert(o.accepted==size && !o.invalid_return && o.drain==ESP_OK);
  unsigned chunks=(unsigned)((size+PET_CONTROL_PREFIX_BYTES+PET_P4_UART_TX_BURST_BYTES-1)/PET_P4_UART_TX_BURST_BYTES);
  assert(calls==(PET_P4_UART_PACING_ACTIVE?chunks+1:2));
  assert(drains==(PET_P4_UART_PACING_ACTIVE?chunks:1));
  assert(delays==(PET_P4_UART_PACING_ACTIVE?chunks:0));
  assert(used==size+PET_CONTROL_PREFIX_BYTES && !memcmp(output,prefix,PET_CONTROL_PREFIX_BYTES));
  assert(!memcmp(output+PET_CONTROL_PREFIX_BYTES,input,size));
#if PET_P4_UART_PACING_ACTIVE
  assert(!g_uart_control_prefix_queued);
#endif
  unsigned before_drains=drains,before_delays=delays;
  uart_write_all("\n",1,&o);
  assert(output[used-1]=='\n' && o.accepted==1 && drains==before_drains+1);
  assert(delays==before_delays+(PET_P4_UART_PACING_ACTIVE?1U:0U));
  /* Odd partial prefix does not create a drain/tick or replay prefix bytes. */
  reset(ODD_FIRST,true);assert(transport_control_prefix(prefix,false));
  assert(calls==2 && used==PET_CONTROL_PREFIX_BYTES && !memcmp(output,prefix,used));
  assert(!drains && !delays);
  /* Failed prefix emits only the existing best-effort newline, also staged. */
  reset(ZERO_FIRST,true);assert(!transport_control_prefix(prefix,false));
  assert(calls==2 && used==1 && output[0]=='\n' && !drains && !delays);
#if PET_P4_UART_PACING_ACTIVE
  assert(!g_uart_control_prefix_queued);
#endif
  reset(FULL,false);assert(transport_control_prefix(prefix,true));
  assert(usb_calls==1 && usb_pointer==prefix && !calls && !drains && !delays);
#if PET_P4_UART_PACING_ACTIVE
  assert(!g_uart_control_prefix_queued);
#endif
}
#endif
int main(void) {
  _Alignas(64) char backing[2048];char *input=backing+1;size_t size=1000;
  for(size_t i=0;i<size;i++) { input[i]=(char)('a'+i%26); }
  input[size]=0;
  assert(((uintptr_t)input%4)==1);
  full_and_odd_partials(input,size);
  terminal_results_never_replay(input,size);
  queue_and_empty_paths(input,size);
  reset(FULL,false);uart_audio_queue_all(input,size);
  assert(used==size&&!memcmp(input,output,size)&&!drains&&!delays);
  reset(ODD_FIRST,false);uart_audio_queue_all(input,size);
  assert(used==size&&!memcmp(input,output,size)&&!drains&&!delays);
#if PET_P4_SERIAL_CONTROL_INTEGRITY
  prefix_debt_and_routes(input,size);
#endif
  return 0;
}
