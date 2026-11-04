#include "pet_p4_boot_dma.h"
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_private/startup_internal.h"
#include "esp_private/periph_ctrl.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "hal/ahb_dma_ll.h"
#include "hal/gdma_ll.h"
_Static_assert(GDMA_LL_AHB_PAIRS_PER_GROUP==PET_BOOT_DMA_CHANNELS,"review AHB layout");
static pet_boot_dma_snapshot_t boot_dma;
_Static_assert(sizeof(boot_dma)<=512,"fixed internal boot evidence budget");

static uint32_t parked(ahb_dma_dev_t *dev){
  uint32_t mask=0;
  for(unsigned i=0;i<PET_BOOT_DMA_CHANNELS;i++){
    if(ahb_dma_ll_rx_is_desc_fsm_idle(dev,i))mask|=1U<<(i*2);
    if(ahb_dma_ll_tx_is_desc_fsm_idle(dev,i))mask|=1U<<(i*2+1);
  }
  return mask;
}
static bool descriptor_word(uint32_t address,uint32_t *word){
  if(!address||(address&3U)||address>UINT32_MAX-3U
      ||!esp_ptr_dma_capable((const void *)(uintptr_t)address)
      ||!esp_ptr_dma_capable((const void *)(uintptr_t)(address+3U)))return false;
  *word=*(volatile const uint32_t *)(uintptr_t)address;return true;
}
static void capture(ahb_dma_dev_t *dev,pet_boot_dma_state_t *out,bool words){
  for(unsigned i=0;i<PET_BOOT_DMA_CHANNELS;i++){
    pet_boot_dma_channel_t *s=&out->channels[i];
    s->rx_link_addr=dev->in_link_addr[i].inlink_addr_chn;
    s->rx_link=dev->channel[i].in.in_link.val;
    s->rx_state=dev->channel[i].in.in_state.val;
    s->rx_peripheral=dev->channel[i].in.in_peri_sel.val;
    s->rx_eof=ahb_dma_ll_rx_get_success_eof_desc_addr(dev,i);
    s->rx_prefetched=ahb_dma_ll_rx_get_prefetched_desc_addr(dev,i);
    s->tx_link_addr=dev->out_link_addr[i].outlink_addr_chn;
    s->tx_link=dev->channel[i].out.out_link.val;
    s->tx_state=dev->channel[i].out.out_state.val;
    s->tx_eof=ahb_dma_ll_tx_get_eof_desc_addr(dev,i);
    s->tx_prefetched=ahb_dma_ll_tx_get_prefetched_desc_addr(dev,i);
    if(words){
      uint32_t addresses[3]={s->rx_link_addr,s->rx_eof,s->rx_prefetched};
      for(unsigned j=0;j<3;j++)if(descriptor_word(addresses[j],&s->rx_dw0[j]))s->rx_dw0_valid|=1U<<j;
    }
  }
  out->parked_mask=parked(dev);
}
/* Both pinned SDKs call CORE init functions in priority order; init_heap=100.
 * At CORE/CPU0, no application driver owns group0 yet. This also protects an
 * incoming upgrade from an older image whose outgoing reset omitted AHB. */
ESP_SYSTEM_INIT_FN(pet_boot_dma_quiesce, CORE, BIT(0), 0){
  boot_dma.version=1;
  boot_dma.clock_was_enabled=gdma_ll_is_bus_clock_enabled(0);
  if(!boot_dma.clock_was_enabled){boot_dma.complete=1;return ESP_OK;}
  ahb_dma_dev_t *dev=AHB_DMA_LL_GET_HW(0);
  capture(dev,&boot_dma.before,true);
  for(unsigned i=0;i<PET_BOOT_DMA_CHANNELS;i++){
    ahb_dma_ll_rx_enable_interrupt(dev,i,UINT32_MAX,false);
    ahb_dma_ll_tx_enable_interrupt(dev,i,UINT32_MAX,false);
    ahb_dma_ll_rx_stop(dev,i);ahb_dma_ll_tx_stop(dev,i);
  }
  for(unsigned i=0;i<PET_BOOT_DMA_STOP_POLLS;i++){
    boot_dma.stopped_mask=parked(dev);boot_dma.stop_polls=i+1;
    if(boot_dma.stopped_mask==0x3fU)break;
    esp_rom_delay_us(1);
  }
  /* P4 legacy periph_module_reset is a no-op. Match native SDK RCC usage;
   * AHB may be pulsed directly (unlike the separately handled AXI masters). */
  PERIPH_RCC_ATOMIC(){gdma_ll_reset_register(0);}
  boot_dma.reset_applied=1;capture(dev,&boot_dma.after,false);
  if(boot_dma.after.parked_mask!=0x3fU)return ESP_ERR_INVALID_STATE;
  for(unsigned i=0;i<PET_BOOT_DMA_CHANNELS;i++){
    if(boot_dma.after.channels[i].rx_link_addr||boot_dma.after.channels[i].tx_link_addr)return ESP_ERR_INVALID_STATE;
  }
  boot_dma.complete=1;return ESP_OK;
}
void pet_boot_dma_link_anchor(void){__asm__ volatile("" ::: "memory");}
bool pet_boot_dma_observe(pet_boot_dma_snapshot_t *out){
  uintptr_t a=(uintptr_t)out,b=(uintptr_t)&boot_dma;
  if(!out||(a<=b?b-a<sizeof(*out):a-b<sizeof(boot_dma)))return false;
  *out=boot_dma;return boot_dma.complete!=0;
}
