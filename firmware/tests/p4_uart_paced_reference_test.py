"""Private comparison and fixed public control pacing; unchanged no-drain path."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_drained_pacing_is_private_bounded_and_keeps_observed_failures(tmp_path):
    source=(ROOT/'main/pet_p4_main.c').read_text()
    function='static void uart_write_all('+source.split('static void uart_write_all(',1)[1].split('static int transport_log_vprintf(',1)[0]
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_transport_audit.h"
#include "pet_p4_uart_pacing.h"
#define PET_P4_UART_NUM 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0
static unsigned calls,drains,delays,mode;
static char output[2048];static size_t used;
static void uart_queue_all(const char*data,size_t size,pet_transport_write_observation_t*o){
 calls++;if(PET_P4_UART_PACING_ACTIVE)assert(size<=PET_P4_UART_TX_BURST_BYTES);
 size_t accepted=mode==1?size/2:size;
 memcpy(output+used,data,accepted);used+=accepted;
 if(o)*o=(pet_transport_write_observation_t){.accepted=accepted,.last_return=(int)accepted,.drain=PET_TRANSPORT_AUDIT_NOT_OBSERVED};
}
/* Preserve this legacy pacing-policy fixture's logical queue abstraction.
 * The companion staging test extracts the REAL control queue and injects
 * partial driver returns there; this fixture retains all prior assertions. */
#if PET_P4_UART_PACING_ACTIVE
static void uart_control_queue_all(const char*data,size_t size,pet_transport_write_observation_t*o){
 uart_queue_all(data,size,o);
}
#endif
static int uart_wait_tx_done(int port,int timeout){assert(port==0&&timeout==100);drains++;return mode==2?7:ESP_OK;}
#if PET_P4_UART_PACING_ACTIVE
static void vTaskDelay(int ticks){assert(ticks==1);delays++;}
#endif
'''+function+r'''
int main(void){
 char input[1000];for(unsigned i=0;i<sizeof(input);i++)input[i]=(char)i;
 pet_transport_write_observation_t o;
 uart_write_all(input,sizeof(input),&o);
 assert(used==sizeof(input)&&!memcmp(input,output,sizeof(input))&&o.accepted==sizeof(input)&&o.drain==ESP_OK);
 unsigned chunks=(1000+PET_P4_UART_TX_BURST_BYTES-1)/PET_P4_UART_TX_BURST_BYTES;
 assert(calls==(PET_P4_UART_PACING_ACTIVE?chunks:1)&&drains==calls&&delays==(PET_P4_UART_PACING_ACTIVE?chunks:0));
 for(mode=1;mode<=2;mode++){
  calls=drains=delays=0;used=0;uart_write_all(input,sizeof(input),&o);
  assert(calls==1&&drains==1&&!delays);
  assert(o.accepted==used&&o.drain==(mode==2?7:ESP_OK));
  if(mode==1)assert(used==(PET_P4_UART_PACING_ACTIVE?PET_P4_UART_TX_BURST_BYTES/2:500));
 }
 return 0;
}
'''
    path=tmp_path/'paced.c';path.write_text(code)
    for enabled,burst,production in ((0,64,0),(1,64,0),(1,256,0),(0,64,1)):
        binary=tmp_path/f'paced-{enabled}-{burst}-{production}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            f'-DPET_P4_UART_CONTROL_PACING={production}',f'-DPET_P4_UART_PACED_REFERENCE={enabled}',f'-DPET_P4_UART_PACED_BYTES={burst}','-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_only_private_profile_enables_pacing_and_default_baud_is_unchanged():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    private='env:esp32_p4_uart_paced_reference'
    assert config[private]['extends']=='env:esp32_p4_canvas_candidate'
    assert config['env:esp32_p4_evboard']['custom_p4_uart_baud']=='4000000'
    for name in config.sections():
        flags=config[name].get('build_flags','')
        if 'PET_P4_UART_PACED_REFERENCE=1' in flags:
            assert name in (private,'env:esp32_p4_uart_paced256_reference','env:esp32_p4_control_paced_probe','env:esp32_p4_framework_mixed_acceptance')
        elif 'PET_P4_UART_PACED_REFERENCE' in flags:
            # The public program candidate rides the private chain and explicitly UNDEFINES the pacing reference.
            assert name=='env:esp32_p4_program_public_candidate' and '-UPET_P4_UART_PACED_REFERENCE' in flags and '-UPET_P4_UART_PACED_BYTES' in flags
    assert config['env:esp32_p4_uart_paced256_reference']['extends']=='env:esp32_p4_canvas_candidate'
    assert config['env:esp32_p4_control_paced_probe']['extends']=='env:esp32_p4_control_integrity_probe'
    assert 'PET_P4_UART_PACED_BYTES=256' in config['env:esp32_p4_control_paced_probe']['build_flags']
    assert config['env:esp32_p4_framework_mixed_acceptance']['extends']=='env:esp32_p4_program_parent_acceptance'
    assert 'PET_P4_UART_PACED_BYTES=256' in config['env:esp32_p4_framework_mixed_acceptance']['build_flags']
    source=(ROOT/'main/pet_p4_main.c').read_text()
    queued=source.split('static void uart_queue_all(',1)[1].split('static void uart_write_all(',1)[0]
    assert 'vTaskDelay(' not in queued and 'uart_wait_tx_done(' not in queued

def test_actual_screenshot_uart_pacing_preserves_deadline_audio_and_short_write_checks(tmp_path):
    from p4_render_trace_test import function
    source=function((ROOT/'main/pet_p4_main.c').read_text(),'screenshot_write_all')
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "pet_p4_screenshot_write.h"
#include "pet_p4_uart_pacing.h"
static pet_screenshot_tx_observation_t g_screenshot_tx;
#define PET_P4_UART_NUM 0
#define pdMS_TO_TICKS(n) (n)
#define ESP_OK 0
typedef uint32_t TickType_t;
static unsigned ticks,calls,drains,delays,mode,used,largest;
static bool audio,revoked;
static char output[8192];
static struct {void *raw;int resources;} g_screenshot;
static bool screenshot_audio_busy(void){return audio;}
static bool pet_resources_device_debug_current(const void *p,bool b){(void)p;(void)b;return !revoked;}
static TickType_t xTaskGetTickCount(void){return ticks;}
#if PET_P4_UART_PACING_ACTIVE
static void vTaskDelay(unsigned n){assert(n==1);ticks+=n;delays++;}
static int uart_wait_tx_done(int p,unsigned timeout){assert(p==0&&timeout==100);drains++;return mode==2?-1:0;}
#endif
static int write_bytes(const char *p,size_t n){
 calls++;if(n>largest)largest=(unsigned)n;
 if(mode==3)return (int)n+1;
 if(mode==4)return 0;
 if(mode==5){audio=true;return (int)n;}
 if(mode==6){revoked=true;return (int)n;}
 if(mode==7)ticks+=50;
 if(mode==1&&calls==1)n/=2;
 assert(used+n<=sizeof(output));memcpy(output+used,p,n);used+=(unsigned)n;return (int)n;
}
static int uart_write_bytes(int p,const void *b,size_t n){assert(!p);return write_bytes(b,n);}
static int usb_serial_jtag_write_bytes(const void *b,size_t n,unsigned timeout){assert(timeout==20);return write_bytes(b,n);}
'''+source+r'''
static void reset(unsigned m){ticks=calls=drains=delays=used=largest=0;audio=revoked=false;mode=m;g_screenshot.raw=(void*)1;g_screenshot_tx=(pet_screenshot_tx_observation_t){0};}
int main(void){
 char input[4096];for(unsigned i=0;i<sizeof(input);i++)input[i]=(char)i;
 for(unsigned m=0;m<=1;m++){
  reset(m);assert(screenshot_write_all(input,sizeof(input),false,0));
  assert(used==sizeof(input)&&!memcmp(output,input,sizeof(input)));
  assert(largest==(PET_P4_UART_PACING_ACTIVE?PET_P4_UART_TX_BURST_BYTES:4096));
  if(PET_P4_UART_PACING_ACTIVE)assert(calls==drains&&delays==calls&&ticks<100);
 }
 reset(0);assert(screenshot_write_all(input,sizeof(input),true,0));assert(calls==1&&!drains&&!delays);
 reset(3);assert(!screenshot_write_all(input,sizeof(input),false,0)&&calls==1&&!used);
 assert(g_screenshot_tx.reason==PET_SCREENSHOT_TX_DRIVER_COUNT&&g_screenshot_tx.accepted==0&&g_screenshot_tx.calls==1);
 reset(4);assert(!screenshot_write_all(input,sizeof(input),false,0)&&calls==1&&!used);
 assert(g_screenshot_tx.reason==PET_SCREENSHOT_TX_DRIVER_ZERO);
 reset(0);audio=true;assert(!screenshot_write_all(input,sizeof(input),false,0)&&!calls);
 assert(g_screenshot_tx.reason==PET_SCREENSHOT_TX_AUDIO);
 reset(0);revoked=true;assert(!screenshot_write_all(input,sizeof(input),false,0)&&!calls);
 assert(g_screenshot_tx.reason==PET_SCREENSHOT_TX_REVOKED);
 reset(0);ticks=100;assert(!screenshot_write_all(input,sizeof(input),false,0)&&!calls);
 assert(g_screenshot_tx.reason==PET_SCREENSHOT_TX_DEADLINE&&g_screenshot_tx.elapsed_ticks==100);
 if(PET_P4_UART_PACING_ACTIVE){
  reset(2);assert(!screenshot_write_all(input,sizeof(input),false,0)&&calls==1&&drains==1&&!delays);
  assert(g_screenshot_tx.reason==PET_SCREENSHOT_TX_DRAIN&&g_screenshot_tx.drain_return==-1);
  reset(5);assert(!screenshot_write_all(input,sizeof(input),false,0)&&calls==1);
  reset(6);assert(!screenshot_write_all(input,sizeof(input),false,0)&&calls==1);
  reset(7);assert(!screenshot_write_all(input,sizeof(input),false,0)&&calls==2&&ticks>=100);
 }
 return 0;
}
'''
    path=tmp_path/'screenshot-pacing.c';path.write_text(code)
    for enabled,burst,production in ((0,64,0),(1,64,0),(1,256,0),(0,64,1)):
        binary=tmp_path/f'screenshot-pacing-{enabled}-{burst}-{production}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
          f'-DPET_P4_UART_CONTROL_PACING={production}',f'-DPET_P4_UART_PACED_REFERENCE={enabled}',f'-DPET_P4_UART_PACED_BYTES={burst}','-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)
