"""Actual no-drain UART audio source isolation; SDK boundary is a memory writer."""
from pathlib import Path
import os,subprocess,json
ROOT=Path(os.environ.get('PET_AUDIO_UART_ROOT',str(Path(__file__).resolve().parents[1])))
MAIN=Path(os.environ.get('PET_AUDIO_UART_MAIN',str(ROOT/'main/pet_p4_main.c')))
def test_actual_audio_uart_stage_partial_suffix_and_bounds(tmp_path):
 source=MAIN.read_text()
 start=source.index('/* Audio only: isolate queued UART copies')
 end=source.index('#if PET_P4_UART_PACING_ACTIVE',start)
 actual=source[start:end]
 code=r'''#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define DRAM_ATTR
#define PET_P4_UART_NUM 0
static unsigned char output[70000];static size_t used,calls,limit,fail_at;static int failure;
static int uart_write_bytes(int port,const void*data,size_t n){
 assert(port==0&&n>0&&n<=256&&((uintptr_t)data%64)==0);calls++;
 if(calls==fail_at)return failure==-2?(int)n+1:failure;
 size_t count=limit&&limit<n?limit:n;assert(used+count<=sizeof(output));
 memcpy(output+used,data,count);used+=count;return (int)count;
}
'''+actual+r'''
int main(void){unsigned cases=0;unsigned char storage[65539];for(size_t i=0;i<sizeof(storage);i++)storage[i]=(unsigned char)(i*31+7);
 const char *source=(const char*)storage+1;
 size_t sizes[]={0,1,2,63,64,255,256,257,640,1200,65536};size_t partials[]={0,1,7,17};
 for(size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++)for(size_t j=0;j<sizeof(partials)/sizeof(partials[0]);j++){
  used=calls=fail_at=0;limit=partials[j];uart_audio_queue_all(source,sizes[i]);assert(used==sizes[i]&&!memcmp(output,source,used));cases++;
 }
 int failures[]={0,-1,-2};for(size_t i=0;i<3;i++){
  used=calls=0;fail_at=2;limit=17;failure=failures[i];uart_audio_queue_all(source,1200);
  assert(calls==2&&used==17&&!memcmp(output,source,used));cases++;
 }
 return cases==47?0:1;
}
'''
 c=tmp_path/'actual.c';c.write_text(code);exe=tmp_path/'actual'
 argv=['/usr/bin/cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)]
 (tmp_path/'command.json').write_text(json.dumps(argv))
 run=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
 (tmp_path/'compile.log').write_text(run.stdout);assert run.returncode==0,run.stdout
 run=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=10)
 (tmp_path/'run.log').write_text(run.stdout);assert run.returncode==0,run.stdout

def test_actual_callsite_keeps_media_native_and_audio_no_drain():
 source=MAIN.read_text()
 start=source.index('static void transport_send_line_impl(')
 # first mention may be its declaration: use body definition.
 if source.find(';',start)<source.find('{',start):start=source.index('static void transport_send_line_impl(',start+1)
 end=source.index('static void transport_send_line(',start)
 actual=source[start:end]
 uart=actual[actual.index('if (route == PET_P4_TRANSPORT_BROADCAST || route == PET_P4_TRANSPORT_UART)'):actual.index('if (route == PET_P4_TRANSPORT_USB_SERIAL_JTAG')]
 assert 'static const char audio_prefix[]="{\\"topic\\":\\"audio/";' in uart
 assert 'uart_audio_queue_all(line,strlen(line))' in uart
 assert 'uart_queue_all(line,strlen(line),NULL)' in uart
 rest=actual[actual.index('if (route == PET_P4_TRANSPORT_USB_SERIAL_JTAG'):]
 assert 'uart_audio_queue_all' not in rest
 stage=source[source.index('static void uart_audio_queue_all'):source.index('#if PET_P4_UART_PACING_ACTIVE',source.index('static void uart_audio_queue_all'))]
 assert 'wait_tx_done' not in stage and 'vTaskDelay' not in stage
