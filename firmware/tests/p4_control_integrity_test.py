"""Real downstream writer prefix and C/Rust vectors, no USB/audio/cloud IO."""
from pathlib import Path
import json
import configparser
import subprocess
import re
ROOT=Path(__file__).resolve().parents[1]
def _c_function(source,name):
    # The real transport embeds intentionally unbalanced JSON string prefixes.
    # Ignore C literals/comments ONLY for brace matching; return original code.
    mask=re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
      lambda m:re.sub(r'[^\n]',' ',m.group()),source)
    match=re.search(r'(?:static )?[\w *]+\b'+re.escape(name)+r'\s*\([^;]*?\)\s*\{',mask)
    assert match,name
    at=mask.index('{',match.start());end=at+1;depth=1
    while depth and end<len(mask):
        depth+=(mask[end]=='{')-(mask[end]=='}');end+=1
    assert not depth,name
    return source[match.start():end]
def test_actual_prefix_matches_shared_raw_utf8_vectors_and_fixed_bounds(tmp_path):
    rows=json.loads((ROOT/'tests/vectors/p4_control_integrity.json').read_text())
    source='#include <assert.h>\n#include <stdlib.h>\n#include <string.h>\n#include "pet_p4_control_integrity.h"\nint main(void){char out[22];size_t n=999;\n'
    for i,row in enumerate(rows):
        raw=row['body'].encode();source+=f'const char s{i}[]={{'+','.join(str(b) for b in raw)+',0};\n'
        expected=f'!P4C1:{row["bytes"]:05}:{row["hash"]}:'
        source+=f'assert(pet_control_prefix(out,s{i},&n)&&n=={len(raw)}&&!strcmp(out,{json.dumps(expected)}));\n'
    source+=r'''
 char *large=malloc(PET_CONTROL_INTEGRITY_MAX+2);assert(large);memset(large,'x',PET_CONTROL_INTEGRITY_MAX+1);large[PET_CONTROL_INTEGRITY_MAX+1]=0;
 n=777;assert(!pet_control_prefix(out,large,&n)&&n==777&&!out[0]);large[PET_CONTROL_INTEGRITY_MAX]=0;
 assert(pet_control_prefix(out,large,&n)&&n==PET_CONTROL_INTEGRITY_MAX&&strlen(out)==21);
 n=888;assert(!pet_control_prefix(out,"",&n)&&n==888&&!out[0]);
 assert(!pet_control_prefix(NULL,"{}",&n));assert(!pet_control_prefix(out,NULL,&n));assert(!pet_control_prefix(out,"{}",NULL));
 free(large);return 0;}
'''
    path=tmp_path/'prefix.c';path.write_text(source);binary=tmp_path/'prefix'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def actual_serial_tx_source():
    function=_c_function
    text=(ROOT/'main/pet_p4_main.c').read_text()
    source=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_transport_audit.h"
#include "pet_p4_control_integrity.h"
#include "pet_p4_uart_pacing.h"
#include "pet_p4_uart_control_staging.h"
#define DRAM_ATTR
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define PET_P4_UART_NUM 0
#define ESP_OK 0
typedef enum {PET_P4_TRANSPORT_BROADCAST,PET_P4_TRANSPORT_UART,PET_P4_TRANSPORT_USB_SERIAL_JTAG} pet_p4_transport_route_t;
static int g_transport_mutex=1;
static unsigned locks,takes,gives,writes[2],drains[2],native_calls;
static char wire[2][66000],native[66000];static size_t used[2];
static bool fail_prefix,failed_once;
static size_t pending_uart;
#if PET_P4_SERIAL_CONTROL_INTEGRITY && PET_P4_UART_PACED_REFERENCE
static size_t g_uart_control_prefix_queued;
#endif
#if PET_P4_UART_PACED_REFERENCE
static void vTaskDelay(unsigned ticks){assert(ticks==1);}
#endif
static pet_transport_audit_t g_transport_audit,g_transport_reply_audit;
static bool g_transport_reply_observed;
static int xSemaphoreTake(int mutex,unsigned timeout){assert(mutex==1&&timeout==portMAX_DELAY&&!locks);locks++;takes++;return pdTRUE;}
static void xSemaphoreGive(int mutex){assert(mutex==1&&locks==1);locks--;gives++;}
static bool usb_protocol_seen(void){return true;}
static int queue(unsigned route,const char *data,size_t length){
 assert(locks==1&&length&&used[route]+length<sizeof(wire[route]));writes[route]++;
 if(fail_prefix&&length>1){if(!failed_once){length=7;failed_once=true;}else return 0;}
 memcpy(wire[route]+used[route],data,length);used[route]+=length;if(!route)pending_uart+=length;return (int)length;
}
static int uart_write_bytes(int port,const void *data,size_t length){assert(port==0);return queue(0,data,length);}
static int usb_serial_jtag_write_bytes(const void *data,size_t length,unsigned timeout){assert(timeout==20);return queue(1,data,length);}
static int uart_wait_tx_done(int port,unsigned timeout){assert(port==0&&timeout==100&&locks==1);
#if PET_P4_UART_PACED_REFERENCE
 assert(pending_uart<=PET_P4_UART_PACED_BYTES);
#endif
 pending_uart=0;drains[0]++;return ESP_OK;}
static int usb_serial_jtag_wait_tx_done(unsigned timeout){assert(timeout==100&&locks==1);drains[1]++;return ESP_OK;}
static void pet_p4_native_usb_send_json_line(const char *line,void *ctx){assert(locks==1&&!ctx);native_calls++;snprintf(native,sizeof(native),"%s",line);}
'''
    for name in ('usb_queue_all','usb_write_all','uart_queue_all'):
        source+=function(text,name)+'\n'
    # Include the REAL gated storage/queue adapter, retaining every original
    # framing, drain, prefix-failure, audio and mutex assertion below.
    queue=function(text,'uart_queue_all')
    after_queue=text.index(queue)+len(queue)
    source+=text[after_queue:text.index('static void uart_write_all(',after_queue)]
    source+=function(text,'uart_write_all')+'\n'
    source+='#if PET_P4_SERIAL_CONTROL_INTEGRITY\n'+function(text,'transport_control_prefix')+'\n#endif\n'
    for name in ('transport_audit_observe','transport_send_line_impl','transport_send_line','transport_send_audio_line'):
        source+=function(text,name)+'\n'
    source+=r'''
static void reset(void){assert(!locks&&takes==gives);memset(wire,0,sizeof(wire));memset(used,0,sizeof(used));memset(writes,0,sizeof(writes));memset(drains,0,sizeof(drains));native_calls=0;fail_prefix=failed_once=false;pending_uart=0;
#if PET_P4_SERIAL_CONTROL_INTEGRITY && PET_P4_UART_PACED_REFERENCE
 assert(!g_uart_control_prefix_queued);
#endif
}
int main(void){
 const char *body="{\"topic\":\"protocol/ack\",\"payload\":{\"ok\":true}}";char expected[66000];
 for(unsigned r=0;r<2;r++){
  reset();pet_p4_transport_route_t route=r?PET_P4_TRANSPORT_USB_SERIAL_JTAG:PET_P4_TRANSPORT_UART;
  transport_send_line(body,&route);
#if PET_P4_SERIAL_CONTROL_INTEGRITY
  char prefix[22];size_t bytes=0;assert(pet_control_prefix(prefix,body,&bytes));snprintf(expected,sizeof(expected),"%s%s\n",prefix,body);
  assert(writes[r]==3&&used[r]==21+strlen(body)+1);
#else
  snprintf(expected,sizeof(expected),"%s\n",body);assert(writes[r]==2);
#endif
  assert(!strcmp(wire[r],expected)&&drains[r]==2&&!writes[1-r]&&!native_calls);
#if PET_P4_SERIAL_CONTROL_INTEGRITY && PET_P4_UART_PACED_REFERENCE
  /* Prefix occupies the SAME first physical control pacing window. A 512B
   * JSON body requires ceil((21+512)/256) body drains, not a 277B first burst. */
  reset();char large[513];memcpy(large,body,strlen(body));memset(large+strlen(body),' ',512-strlen(body));large[512]=0;
  transport_send_line(large,&route);assert(used[r]==21+512+1&&drains[r]==(r?2:4));
#endif
  /* No prefix/hash/drain path for audio/media or explicit audio sender. */
  for(unsigned audio=0;audio<3;audio++){
   reset();const char *line=audio==0?"{\"topic\":\"audio/frame\"}":"{\"topic\":\"media/ack\"}";
   if(audio==2)transport_send_audio_line(line,&route);else transport_send_line(line,&route);
   snprintf(expected,sizeof(expected),"%s\n",line);assert(!strcmp(wire[r],expected)&&writes[r]==2&&!drains[r]);
  }
#if PET_P4_SERIAL_CONTROL_INTEGRITY
  reset();fail_prefix=true;transport_send_line(body,&route);
  assert(failed_once&&used[r]==8&&wire[r][7]=='\n'&&!drains[r]&&!strstr(wire[r],"protocol/ack"));
#endif
 }
 reset();transport_send_line(body,NULL);assert(native_calls==1&&!strcmp(native,body));
 assert(writes[0]==writes[1]&&drains[0]==2&&drains[1]==2&&!locks&&takes==gives);
 assert(!g_transport_audit.seen&&!g_transport_reply_audit.seen&&!g_transport_reply_observed);
 return 0;
}
'''
    return source

def test_actual_serial_tx_single_mutex_existing_drains_native_and_audio_unchanged(tmp_path):
    path=tmp_path/'tx.c';path.write_text(actual_serial_tx_source())
    for enabled,paced in ((0,0),(1,0),(1,1)):
        binary=tmp_path/f'tx-{enabled}-{paced}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DPET_P4_SERIAL_CONTROL_INTEGRITY={enabled}',
          f'-DPET_P4_UART_PACED_REFERENCE={paced}','-DPET_P4_UART_PACED_BYTES=256',
          '-I',str(ROOT/'main'),str(path),str(ROOT/'main/pet_p4_transport_audit.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_control_integrity_is_private_and_snapshot_writer_stays_independent():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_control_integrity_probe':
            assert config[section]['extends']=='env:esp32_p4_canvas_candidate' and 'PET_P4_SERIAL_CONTROL_INTEGRITY=1' in flags
        elif section=='env:esp32_p4_framework_mixed_acceptance':
            assert config[section]['extends']=='env:esp32_p4_program_parent_acceptance' and 'PET_P4_SERIAL_CONTROL_INTEGRITY=1' in flags
            assert 'PET_P4_UART_PACED_BYTES=256' in flags
        else:assert 'PET_P4_SERIAL_CONTROL_INTEGRITY=1' not in flags
    from p4_render_trace_test import function
    text=(ROOT/'main/pet_p4_main.c').read_text()
    assert 'transport_control_prefix' not in function(text,'screenshot_send_parts')
    assert 'transport_control_prefix' not in function(text,'screenshot_write_all')
    assert '#define PET_P4_SERIAL_CONTROL_INTEGRITY 0' in (ROOT/'main/pet_p4_control_integrity.h').read_text()

def test_live_reader_enforces_negotiation_before_any_waiter_and_logs_metadata_only():
    text=(ROOT.parent/'pc/src-tauri/src/usb_serial.rs').read_text()
    reader=text.split('fn read_connected<',1)[1].split('// Reader thread ended',1)[0]
    assert reader.index('control_integrity::permitted(')<reader.index('resolve_asset_ack(')
    assert reader.index('control_integrity::permitted(')<reader.index('resolve_device_response(')
    assert 'require_control_integrity|=claims' in reader and '(require_control_integrity&&!claims)' in reader
    raw=text.split('fn read_serial_frame<',1)[1].split('// Existing byte-fragment',1)[0]
    assert raw.index('control_integrity::decode(buffer)')<raw.index('std::str::from_utf8(body)')
    log=text.split('fn log_serial_integrity_failure(',1)[1].split('fn validate_expected_board_device_id',1)[0]
    for forbidden in ('frame.text','payload','fingerprint','boardDeviceId','requestId','filePath','eprintln!'):
        assert forbidden not in log
    assert 'is_power_of_two()' in log and 'saturating_add(1)' in log
