"""Run production screenshot writer/capture with offline TX fault injection."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_screenshot_short_write_encode_and_drain_fail_closed(tmp_path):
    _run_screenshot_writer(tmp_path,False)

def test_optin_close_receipt_binds_route_id_request_and_physically_retires_before_ack(tmp_path):
    _run_screenshot_writer(tmp_path,True)

def test_optin_close_receipt_keeps_negotiated_control_prefix_not_raw_image_exemption(tmp_path):
    _run_screenshot_writer(tmp_path,True,True)

def test_negotiated_close_ack_preserves_legacy_and_framed_routes(tmp_path):
    for framed in (False,True):
        folder=tmp_path/str(framed);folder.mkdir()
        _run_screenshot_writer(folder,True,True,negotiated=True,framed=framed)

def _run_screenshot_writer(tmp_path,close_ack,integrity=False,negotiated=False,framed=True):
    source = (ROOT / 'main/pet_p4_main.c').read_text()
    production = source.split('/* Screenshot-only checked writes.', 1)[1]
    production = production.split('/* Only protocol_worker_task owns these scratch buffers.', 1)[0]
    production = '/* Screenshot-only checked writes.' + production
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_resources_device.h"
#define PET_P4_UART_NUM 0
#define PET_P4_SCREENSHOT_W 320
#define PET_P4_SCREENSHOT_H 240
#define PET_P4_SCREENSHOT_RAW_CHUNK 3072
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ESP_OK 0
typedef uint32_t TickType_t;
typedef int SemaphoreHandle_t;
typedef enum {PET_P4_TRANSPORT_BROADCAST,PET_P4_TRANSPORT_UART,PET_P4_TRANSPORT_USB_SERIAL_JTAG} pet_p4_transport_route_t;
static SemaphoreHandle_t g_transport_mutex=1,g_render_mutex=2;
static char wire[2000000];static size_t used,fail_after=SIZE_MAX;
static unsigned ticks,drains,fail_drain=UINT32_MAX;
static bool encoding_error,audio_busy,tx_lock_error,allocation_error;
static int locks;
static uint16_t framebuffer[640*480];
static TickType_t xTaskGetTickCount(void){return ticks;}
static void vTaskDelay(int delay){ticks+=(unsigned)delay;}
static int64_t esp_timer_get_time(void){return 1000000;}
static const char *pet_p4_boot_identity(void){return "01020304aabbccdd";}
static bool pet_p4_audio_active(void){return audio_busy;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
static bool usb_protocol_seen(void){return true;}
#if PET_P4_SERIAL_CONTROL_NEGOTIATED && PET_P4_SERIAL_CONTROL_INTEGRITY
static bool pet_control_session_framed(const void *route){assert(route);return TEST_FRAMED;}
#endif
static int xSemaphoreTake(SemaphoreHandle_t m,int timeout){(void)timeout;if(m==1&&tx_lock_error)return 0;locks++;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t m){(void)m;assert(locks>0);locks--;}
static void *heap_caps_malloc(size_t n,int cap){(void)cap;return allocation_error?NULL:malloc(n);}
static const uint16_t *pet_p4_renderer_logical_framebuffer(int *w,int *h){*w=640;*h=480;return framebuffer;}
static int write_bytes(const char *p,size_t n){
  if(used>=fail_after){fail_after=SIZE_MAX;return 0;}
  if(n>17)n=17;
  if(n>fail_after-used)n=fail_after-used;
  assert(used+n<sizeof(wire));memcpy(wire+used,p,n);used+=n;wire[used]=0;return (int)n;
}
static int usb_serial_jtag_write_bytes(const void *p,size_t n,int timeout){(void)timeout;return write_bytes(p,n);}
static int uart_write_bytes(int port,const void *p,size_t n){(void)port;return write_bytes(p,n);}
static int usb_serial_jtag_wait_tx_done(int timeout){(void)timeout;return drains++==fail_drain?-1:0;}
static int uart_wait_tx_done(int port,int timeout){(void)port;return usb_serial_jtag_wait_tx_done(timeout);}
static int mbedtls_base64_encode(unsigned char *out,size_t capacity,size_t *length,const uint8_t *raw,size_t n){
  if(encoding_error)return -1;
  static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  *length=((n+2)/3)*4;if(*length>capacity)return -1;
  size_t at=0;
  for(size_t i=0;i<n;i+=3){
    unsigned v=(unsigned)raw[i]<<16;if(i+1<n)v|=(unsigned)raw[i+1]<<8;if(i+2<n)v|=raw[i+2];
    out[at++]=alphabet[(v>>18)&63];out[at++]=alphabet[(v>>12)&63];
    out[at++]=i+1<n?alphabet[(v>>6)&63]:'=';out[at++]=i+2<n?alphabet[v&63]:'=';
  }
  return 0;
}
'''
    harness += production + r'''
static void reset(void){screenshot_release();used=ticks=drains=0;wire[0]=0;fail_after=SIZE_MAX;fail_drain=UINT32_MAX;encoding_error=audio_busy=tx_lock_error=allocation_error=false;assert(locks==0);}
static void check_success(void){
  unsigned chunks=0,ends=0;char *save;
  for(char *line=strtok_r(wire,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
    cJSON *j=cJSON_Parse(line);assert(j);cJSON *p=cJSON_GetObjectItemCaseSensitive(j,"payload");
    const char *topic=cJSON_GetObjectItemCaseSensitive(j,"topic")->valuestring;
    if(!strcmp(topic,"debug/screenshot_chunk")){
      assert(cJSON_GetObjectItemCaseSensitive(p,"index")->valueint==(int)chunks);
      assert(cJSON_GetObjectItemCaseSensitive(p,"rawSize")->valueint==3072);
      assert(strlen(cJSON_GetObjectItemCaseSensitive(p,"data")->valuestring)==4096);
      uint8_t bytes[3072];for(unsigned i=0;i<1536;i++){uint16_t px=framebuffer[chunks*1536+i];bytes[i*2]=px&255;bytes[i*2+1]=px>>8;}
      char checksum[9];snprintf(checksum,sizeof(checksum),"%08x",screenshot_chunk_checksum(bytes,sizeof(bytes)));
      assert(!strcmp(checksum,cJSON_GetObjectItemCaseSensitive(p,"checksum")->valuestring));chunks++;
    }else if(!strcmp(topic,"debug/screenshot_end")){
      assert(cJSON_GetObjectItemCaseSensitive(p,"totalBytes")->valueint==614400);
      assert(cJSON_GetObjectItemCaseSensitive(p,"chunks")->valueint==200);ends++;
    }else {assert(!strcmp(topic,"debug/screenshot_begin"));assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"bootId")->valuestring,"01020304aabbccdd"));}
    cJSON_Delete(j);
  }
  assert(chunks==200&&ends==1&&drains==202&&locks==0);
}
int main(void){
  for(unsigned i=0;i<640*480;i++)framebuffer[i]=(uint16_t)i;
  pet_p4_transport_route_t route=PET_P4_TRANSPORT_USB_SERIAL_JTAG;
  const char *command="{\"topic\":\"debug/screenshot\",\"payload\":{\"fullResolution\":true}}";
  reset();assert(maybe_handle_debug_line(command,&route));check_success();
  // Partial positive writes are retried; a zero write mid-base64 cannot be
  // papered over with a JSON suffix and a successful end marker.
  reset();fail_after=1000;assert(maybe_handle_debug_line(command,&route));
  assert(strstr(wire,"screenshot_error")&&!strstr(wire,"screenshot_end"));assert(locks==0);
  reset();fail_after=5;assert(maybe_handle_debug_line(command,&route));
  assert(strstr(wire,"screenshot_error")&&!strstr(wire,"screenshot_chunk")&&!strstr(wire,"screenshot_end"));
  reset();encoding_error=true;assert(maybe_handle_debug_line(command,&route));
  assert(strstr(wire,"screenshot_error")&&!strstr(wire,"screenshot_chunk")&&!strstr(wire,"screenshot_end"));
  reset();fail_drain=1;assert(maybe_handle_debug_line(command,&route));
  assert(strstr(wire,"screenshot_error")&&!strstr(wire,"screenshot_end"));
  reset();route=PET_P4_TRANSPORT_UART;fail_after=1000;assert(maybe_handle_debug_line(command,&route));
  assert(strstr(wire,"screenshot_error")&&!strstr(wire,"screenshot_end"));
  reset();audio_busy=true;assert(maybe_handle_debug_line(command,&route));assert(used==0&&drains==0);
  reset();tx_lock_error=true;assert(maybe_handle_debug_line(command,&route));assert(used==0&&drains==0);
  reset();allocation_error=true;assert(maybe_handle_debug_line(command,&route));
  assert(strstr(wire,"no memory")&&!strstr(wire,"screenshot_begin")&&!strstr(wire,"screenshot_end"));
  reset();assert(!maybe_handle_debug_line("{\"topic\":\"other\",\"text\":\"debug/screenshot\"}",&route));assert(used==0);
  reset();assert(!maybe_handle_debug_line("{\"topic\":\"debug/screenshot\",\"payload\":{}} trailing",&route));
  assert(!used&&!drains&&!g_screenshot.raw);
  // Pull mode retains one immutable image and answers only the requested block.
  // Lost/corrupt responses can be re-read; no duplicate capture or mutation.
  const char *pull="{\"topic\":\"debug/screenshot\",\"payload\":{\"fullResolution\":true,\"mode\":\"pull-v1\",\"requestId\":\"offline-1\"}}";
  reset();assert(maybe_handle_debug_line(pull,&route));
  assert(g_screenshot.raw&&g_screenshot.pull&&drains==1&&!strstr(wire,"screenshot_chunk"));
  char identity[32];snprintf(identity,sizeof(identity),"%s",g_screenshot.id);
  uint8_t *original=g_screenshot.raw;
  assert(maybe_handle_debug_line(pull,&route));assert(g_screenshot.raw==original&&!strcmp(identity,g_screenshot.id));
  assert(maybe_handle_debug_line(command,&route));assert(g_screenshot.raw==original&&strstr(wire,"snapshot busy"));
  char read[256];snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":23}}",identity);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"screenshot_chunk"));
  char first[5000];size_t first_size=strlen(wire);assert(first_size<sizeof(first));memcpy(first,wire,first_size+1);
  framebuffer[0]^=1;used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(!strcmp(first,wire));framebuffer[0]^=1;
  pet_p4_transport_route_t other=PET_P4_TRANSPORT_USB_SERIAL_JTAG;
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&other));assert(strstr(wire,"mismatched")&&!strstr(wire,"screenshot_chunk"));
  assert(g_screenshot.raw==original);
  ticks+=PET_P4_SCREENSHOT_PULL_IDLE_MS;screenshot_poll();assert(!g_screenshot.raw);
  assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"snapshot idle expired"));
  reset();assert(maybe_handle_debug_line(pull,&route));audio_busy=true;screenshot_poll();assert(!g_screenshot.raw);
  assert(g_screenshot_terminal.reason==SCREENSHOT_RELEASE_REVOKED);
  reset();assert(maybe_handle_debug_line(pull,&route));
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":200}}",g_screenshot.id);
  assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"screenshot_end"));
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_close\",\"payload\":{\"id\":\"%s\"}}",g_screenshot.id);
  assert(maybe_handle_debug_line(read,&route));assert(!g_screenshot.raw);
  assert(g_screenshot_terminal.reason==SCREENSHOT_RELEASE_CLOSED);
  pet_resource_usage_t resources;pet_resources_device_usage(&resources);assert(resources.leases[PET_RESOURCE_SNAPSHOT]==0&&resources.leases[PET_RESOURCE_DEBUG_BULK]==0);
  reset();assert(maybe_handle_debug_line(pull,&route));
  ticks=PET_P4_SCREENSHOT_PULL_LIFETIME_MS;g_screenshot.touched=ticks;screenshot_poll();assert(!g_screenshot.raw);
  assert(g_screenshot_terminal.reason==SCREENSHOT_RELEASE_LIFETIME);
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":0}}",g_screenshot_terminal.id);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"snapshot lifetime expired"));
  // Real realtime reservation revokes the snapshot before delayed audio flags.
  reset();assert(maybe_handle_debug_line(pull,&route));pet_resource_realtime_t reserved={0};
  assert(pet_resources_device_realtime_begin(&reserved)==PET_RESOURCE_OK);
  assert(!audio_busy);screenshot_poll();assert(!g_screenshot.raw&&g_screenshot_terminal.reason==SCREENSHOT_RELEASE_REVOKED);
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":0}}",g_screenshot_terminal.id);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"snapshot lease revoked"));
  pet_resources_device_realtime_end(&reserved);
  // A successful byte delivery followed by failed drain remains a TX failure,
  // not a guessed expiry or permission to recapture another frame.
  reset();assert(maybe_handle_debug_line(pull,&route));fail_drain=drains;
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":0}}",g_screenshot.id);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));
  assert(!g_screenshot.raw&&g_screenshot_terminal.reason==SCREENSHOT_RELEASE_TRANSPORT);
  assert(g_screenshot_terminal.tx.reason==PET_SCREENSHOT_TX_DRAIN);
  assert(g_screenshot_terminal.tx.drain_return==-1&&g_screenshot_terminal.tx.accepted>4096);
  fail_drain=UINT32_MAX;used=0;wire[0]=0;
  assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"snapshot transport failed"));
  assert(strstr(wire,"\"writerReason\":6")&&strstr(wire,"\"writerDrain\":-1"));
  // Tick wrap does not invent expiry; retained terminal details cannot disclose
  // another route's history and an ID mismatch cannot steal the active image.
  reset();ticks=UINT32_MAX-100;assert(maybe_handle_debug_line(pull,&route));ticks+=200;screenshot_poll();assert(g_screenshot.raw);
  used=0;wire[0]=0;assert(maybe_handle_debug_line("{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"other\",\"index\":0}}",&route));
  assert(strstr(wire,"snapshot id mismatched")&&g_screenshot.raw);
  ticks+=PET_P4_SCREENSHOT_PULL_IDLE_MS;screenshot_poll();assert(!g_screenshot.raw);
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":0}}",g_screenshot_terminal.id);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&other));
  assert(strstr(wire,"snapshot expired or mismatched")&&!strstr(wire,"snapshot idle expired"));
  assert(strstr(wire,"\"writerReason\":0")&&!strstr(wire,"\"writerDrain\":-1"));
  reset();assert(maybe_handle_debug_line(pull,&route));g_screenshot.reads=800;
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":0}}",g_screenshot.id);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"invalid snapshot read")&&!strstr(wire,"screenshot_chunk"));reset();
  // Transient TX-lock contention before ANY byte is written must not turn an
  // immutable read into a terminal failure. No timer/budget or nonce changes.
  reset();tx_lock_error=true;assert(maybe_handle_debug_line(pull,&route));
  assert(g_screenshot.raw&&g_screenshot.pull&&!used&&!locks&&g_screenshot_tx.reason==PET_SCREENSHOT_TX_MUTEX);
  uint8_t *busy_frame=g_screenshot.raw;uint32_t busy_sequence=g_screenshot_sequence;
  tx_lock_error=false;assert(maybe_handle_debug_line(pull,&route));
  assert(g_screenshot.raw==busy_frame&&g_screenshot_sequence==busy_sequence&&strstr(wire,"screenshot_begin"));
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_read\",\"payload\":{\"id\":\"%s\",\"index\":0}}",g_screenshot.id);
  used=0;wire[0]=0;tx_lock_error=true;assert(maybe_handle_debug_line(read,&route));
  assert(g_screenshot.raw==busy_frame&&g_screenshot.reads==1&&!used&&!locks);
  tx_lock_error=false;assert(maybe_handle_debug_line(read,&route));
  assert(g_screenshot.raw==busy_frame&&g_screenshot.reads==2&&strstr(wire,"screenshot_chunk")&&g_screenshot_sequence==busy_sequence);
  ticks=PET_P4_SCREENSHOT_PULL_IDLE_MS;screenshot_poll();assert(!g_screenshot.raw);
#if PET_P4_SCREENSHOT_CLOSE_ACK
  reset();assert(maybe_handle_debug_line(pull,&route));original=g_screenshot.raw;
  snprintf(identity,sizeof(identity),"%s",g_screenshot.id);
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_close\",\"payload\":{\"id\":\"%s\",\"requestId\":\"wrong\",\"ack\":true}}",identity);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));
  assert(g_screenshot.raw==original&&strstr(wire,"invalid snapshot close")&&!strstr(wire,"screenshot_closed"));
  assert(g_screenshot_rx.received_hash==g_screenshot_rx.expected_hash&&g_screenshot_rx.received_bytes==strlen(identity));
  assert(g_screenshot_rx.bytes==strlen(read)&&g_screenshot_rx.hash==screenshot_chunk_checksum((const uint8_t*)read,strlen(read)));
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_close\",\"payload\":{\"id\":\"%s\",\"requestId\":\"offline-1\",\"ack\":\"true\"}}",identity);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(g_screenshot.raw==original&&!strstr(wire,"screenshot_closed"));
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_close\",\"payload\":{\"id\":\"%s\",\"requestId\":\"offline-1\",\"ack\":true}}",identity);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&other));assert(g_screenshot.raw==original&&!strstr(wire,"screenshot_closed"));
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(!g_screenshot.raw&&strstr(wire,"screenshot_closed"));
  const char *closed_json=wire;
#if PET_P4_SERIAL_CONTROL_INTEGRITY && (!PET_P4_SERIAL_CONTROL_NEGOTIATED || TEST_FRAMED)
  assert(!strncmp(wire,"!P4C1:",6));closed_json=wire+PET_CONTROL_PREFIX_BYTES;
  char prefix[22];size_t closed_bytes=0;char *newline=strchr((char*)closed_json,'\n');assert(newline);*newline=0;
  assert(pet_control_prefix(prefix,closed_json,&closed_bytes)&&!strncmp(wire,prefix,PET_CONTROL_PREFIX_BYTES));*newline='\n';
#endif
  cJSON *closed=cJSON_Parse(closed_json);assert(closed);cJSON *payload=cJSON_GetObjectItemCaseSensitive(closed,"payload");
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload,"closed")));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(payload,"id")->valuestring,identity));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(payload,"requestId")->valuestring,"offline-1"));cJSON_Delete(closed);
  pet_resources_device_usage(&resources);assert(!resources.leases[PET_RESOURCE_SNAPSHOT]&&!resources.leases[PET_RESOURCE_DEBUG_BULK]);
  used=0;wire[0]=0;assert(maybe_handle_debug_line(read,&route));assert(strstr(wire,"snapshot already closed")&&!strstr(wire,"screenshot_closed"));
  // Refusing a changed id never releases somebody else's snapshot. Correlation
  // contains only bounded numbers, never arbitrary request extension text.
  reset();assert(maybe_handle_debug_line(pull,&route));original=g_screenshot.raw;
  used=0;wire[0]=0;assert(maybe_handle_debug_line("{\"topic\":\"debug/screenshot_close\",\"payload\":{\"id\":\"changed\",\"requestId\":\"offline-1\",\"ack\":true}}",&route));
  assert(g_screenshot.raw==original&&g_screenshot_rx.received_hash!=g_screenshot_rx.expected_hash);
  closed=cJSON_Parse(wire);assert(closed);cJSON_Delete(closed);
  snprintf(read,sizeof(read),"{\"topic\":\"debug/screenshot_close\",\"payload\":{\"id\":\"%s\",\"requestId\":\"offline-1\",\"ack\":true}}",g_screenshot.id);
  used=0;wire[0]=0;fail_drain=drains;assert(maybe_handle_debug_line(read,&route));
  assert(!g_screenshot.raw&&strstr(wire,"screenshot_closed")&&strstr(wire,"snapshot close acknowledgement failed"));
  assert(strstr(wire,"\"writerReason\":6"));reset();
#endif
  puts("PASS screenshot checked TX: immutable bounded mutex reread, 200 chunks, partial/zero write, encode/drain failure, audio exclusion");
}
'''
    source_path, exe = tmp_path / 'screenshot.c', tmp_path / 'screenshot'
    source_path.write_text(harness)
    (tmp_path / 'freertos').mkdir()
    (tmp_path / 'freertos/FreeRTOS.h').write_text(
        '#pragma once\n#include <pthread.h>\n'
        'typedef pthread_mutex_t portMUX_TYPE;\n'
        '#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n'
        '#define portENTER_CRITICAL(m) ((void)pthread_mutex_lock(m))\n'
        '#define portEXIT_CRITICAL(m) ((void)pthread_mutex_unlock(m))\n')
    vendor = ROOT / 'tests/vendor/cjson'
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                    f'-DPET_P4_SCREENSHOT_CLOSE_ACK={int(close_ack)}',
                    f'-DPET_P4_SERIAL_CONTROL_INTEGRITY={int(integrity)}',
                    f'-DPET_P4_SERIAL_CONTROL_NEGOTIATED={int(negotiated)}',f'-DTEST_FRAMED={int(framed)}',
                    '-Werror', '-Wno-error=deprecated-declarations', '-fsanitize=address,undefined',
                    '-pthread', '-I', str(tmp_path), '-I', str(ROOT / 'main'),
                    '-I', str(vendor), str(source_path), str(vendor / 'cJSON.c'),
                    str(ROOT / 'main/pet_p4_resources.c'), str(ROOT / 'main/pet_p4_resources_device.c'),
                    '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
