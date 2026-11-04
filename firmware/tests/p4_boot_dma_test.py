"""Actual early guard with synthetic MMIO; no device or PIO build."""
from pathlib import Path
import importlib.util,subprocess,hashlib,os
import pytest
ROOT=Path(__file__).resolve().parents[1]
V1=Path(os.environ.get('PLATFORMIO_CORE_DIR',str(Path.home()/'.platformio')))/'packages/framework-espidf'
def v3_idf():
    override=os.environ.get('PET_P4_V3_IDF_PATH')
    if override:
        path=Path(override).expanduser().resolve()
        assert path.is_dir(), 'PET_P4_V3_IDF_PATH must name the existing exact SDK'
        return path
    # Same deterministic isolated-SDK discovery as the real object checks.
    candidates=sorted(ROOT.parents[2].glob('*/v3-isolated-exact-*/core/packages/framework-espidf'))
    assert len(candidates)==1, 'set PET_P4_V3_IDF_PATH to the existing exact v3 SDK; no arbitrary SDK fallback'
    return candidates[0]
def patcher():
    spec=importlib.util.spec_from_file_location('restart_patch',ROOT/'tools/patch_p4_restart_ahb.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def test_pinned_v1_copy_exact_native_pair_input_immutable(tmp_path):
    p=patcher();source=V1/p.REL;before=source.read_bytes();dest=p.select(V1,tmp_path)
    assert dest.is_relative_to(tmp_path) and dest!=source and source.read_bytes()==before
    data=dest.read_bytes();assert data.decode().count(p.SET)==data.decode().count(p.CLEAR)==1
    assert data.replace(p.SET.encode(),b'').replace(p.CLEAR.encode(),b'')==before
    assert p.select(V1,tmp_path)==dest

def test_pinned_v3_native_unchanged_and_unknown_rejected_before_output(tmp_path):
    p=patcher();V3=v3_idf();source=V3/p.REL;before=source.read_bytes()
    assert p.select(V3,tmp_path)==source and source.read_bytes()==before and not list(tmp_path.iterdir())
    with pytest.raises(ValueError,match='unrecognized'):p.transform(before+b'\n')
    with pytest.raises(ValueError,match='separate build'):p.select(V1,V1/'not-created-private-output')
    assert not (V1/'not-created-private-output').exists()

def test_actual_core_hook_clock_guard_capture_before_reset_bounded_and_fail_closed(tmp_path):
    inc=tmp_path/'inc';inc.mkdir()
    (inc/'fake.h').write_text(r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define BIT(x) (1U<<(x))
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 7
#define GDMA_LL_AHB_PAIRS_PER_GROUP 3
#define ESP_SYSTEM_INIT_FN(f,stage,core,prio) enum{hook_prio=prio,hook_core=core}; static int __esp_system_init_fn_##f(void)
typedef union {uint32_t val;uint32_t inlink_addr_chn;uint32_t outlink_addr_chn;} reg_t;
typedef struct {reg_t in_link,in_state,in_peri_sel;} rx_t;
typedef struct {reg_t out_link,out_state;} tx_t;
typedef struct {rx_t in;tx_t out;} channel_t;
typedef struct {channel_t channel[3];reg_t in_link_addr[3],out_link_addr[3];} ahb_dma_dev_t;
static ahb_dma_dev_t hw;static bool clock_on,stuck,reset_bad;static unsigned touches,resets,stops,polls,rcc;
#define AHB_DMA_LL_GET_HW(id) (&hw)
#define PERIPH_RCC_ATOMIC() for(unsigned once=(++rcc,1);once;once=0,--rcc)
static bool gdma_ll_is_bus_clock_enabled(unsigned g){assert(g==0);return clock_on;}
static bool esp_ptr_dma_capable(const void*p){(void)p;return false;}
static bool ahb_dma_ll_rx_is_desc_fsm_idle(ahb_dma_dev_t*d,unsigned i){assert(clock_on&&d==&hw);touches++;return !stuck&&(!stops||stops>=6);}
static bool ahb_dma_ll_tx_is_desc_fsm_idle(ahb_dma_dev_t*d,unsigned i){(void)i;return ahb_dma_ll_rx_is_desc_fsm_idle(d,0);}
static uint32_t ahb_dma_ll_rx_get_success_eof_desc_addr(ahb_dma_dev_t*d,unsigned i){(void)d;touches++;return 0x4ff3ad40U+i*64;}
static uint32_t ahb_dma_ll_rx_get_prefetched_desc_addr(ahb_dma_dev_t*d,unsigned i){return ahb_dma_ll_rx_get_success_eof_desc_addr(d,i);}
static uint32_t ahb_dma_ll_tx_get_eof_desc_addr(ahb_dma_dev_t*d,unsigned i){(void)d;(void)i;return 0;}
static uint32_t ahb_dma_ll_tx_get_prefetched_desc_addr(ahb_dma_dev_t*d,unsigned i){(void)d;(void)i;return 0;}
static void ahb_dma_ll_rx_enable_interrupt(ahb_dma_dev_t*d,unsigned i,uint32_t m,bool on){(void)d;(void)i;(void)m;assert(!on);}
static void ahb_dma_ll_tx_enable_interrupt(ahb_dma_dev_t*d,unsigned i,uint32_t m,bool on){ahb_dma_ll_rx_enable_interrupt(d,i,m,on);}
static void ahb_dma_ll_rx_stop(ahb_dma_dev_t*d,unsigned i){(void)d;(void)i;stops++;}
static void ahb_dma_ll_tx_stop(ahb_dma_dev_t*d,unsigned i){(void)d;(void)i;stops++;}
static void esp_rom_delay_us(unsigned n){assert(n==1);polls++;}
static void gdma_ll_reset_register(unsigned group){assert(group==0&&rcc==1&&stops==6);resets++;memset(&hw,0,sizeof(hw));stuck=reset_bad;}
''')
    for name in ['sdkconfig.h','esp_private/startup_internal.h','esp_private/periph_ctrl.h','esp_memory_utils.h','esp_rom_sys.h','hal/ahb_dma_ll.h','hal/gdma_ll.h']:
        p=inc/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "fake.h"\n')
    src=tmp_path/'guard.c';src.write_text(r'''
#include <stdlib.h>
#include "pet_p4_boot_dma.c"
int main(int argc,char**argv){
 assert(argc==2&&hook_prio==0&&hook_core==1);unsigned mode=atoi(argv[1]);clock_on=mode!=0;stuck=mode>=2;reset_bad=mode==3;
 hw.in_link_addr[0].val=0x4ff3ad40;hw.channel[0].in.in_state.val=0x1234;
 int result=__esp_system_init_fn_pet_boot_dma_quiesce();pet_boot_dma_snapshot_t s;
 if(mode==3){assert(result==ESP_ERR_INVALID_STATE&&!pet_boot_dma_observe(&s));return 0;}
 assert(result==ESP_OK&&pet_boot_dma_observe(&s));assert(!pet_boot_dma_observe(NULL));assert(!pet_boot_dma_observe(&boot_dma));
 if(!mode){assert(!touches&&!resets&&!stops&&!s.reset_applied);return 0;}
 assert(resets==1&&stops==6&&s.reset_applied&&s.before.channels[0].rx_link_addr==0x4ff3ad40);
 assert(s.before.channels[0].rx_state==0x1234&&s.before.channels[0].rx_eof==0x4ff3ad40);
 assert(!s.before.channels[0].rx_dw0_valid&&s.after.parked_mask==0x3f&&!s.after.channels[0].rx_link_addr);
 assert(mode!=2||polls==PET_BOOT_DMA_STOP_POLLS);assert(rcc==0);return 0;
}
''')
    binary=tmp_path/'guard';subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-I',str(inc),'-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    for mode in range(4):subprocess.run([str(binary),str(mode)],check=True,timeout=5)
