"""Production checked TX boundaries, without a serial device or audio IO."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-pthread"]


def test_screenshot_realtime_revocation_before_drain_retains_physical_buffers(tmp_path):
    source = (ROOT / "main/pet_p4_main.c").read_text()
    production = "/* Screenshot-only checked writes." + source.split(
        "/* Screenshot-only checked writes.", 1)[1].split("static bool send_screenshot_chunk(", 1)[0]
    source_path = tmp_path / "screenshot_reservation.c"
    source_path.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_resources_device.h"
#define PET_P4_UART_NUM 0
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ESP_OK 0
typedef uint32_t TickType_t;
typedef int SemaphoreHandle_t;
typedef enum {PET_P4_TRANSPORT_BROADCAST,PET_P4_TRANSPORT_UART,PET_P4_TRANSPORT_USB_SERIAL_JTAG} pet_p4_transport_route_t;
static SemaphoreHandle_t g_transport_mutex=1;
static unsigned ticks,writes,drains,locks,inject_at;
static pet_resource_realtime_t realtime;
static TickType_t xTaskGetTickCount(void){return ticks;}
static bool pet_p4_audio_active(void){return false;}
static bool pet_p4_audio_playback_active(void){return false;}
static bool pet_p4_audio_conversation_active(void){return false;}
static bool pet_p4_audio_music_active(void){return false;}
static bool usb_protocol_seen(void){return true;}
static int xSemaphoreTake(SemaphoreHandle_t m,int timeout){(void)m;(void)timeout;locks++;return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t m){(void)m;assert(locks==1);locks--;}
static int write_bytes(const char *data,size_t count);
static int uart_write_bytes(int port,const void *data,size_t count){(void)port;return write_bytes(data,count);}
static int usb_serial_jtag_write_bytes(const void *data,size_t count,int timeout){(void)timeout;return write_bytes(data,count);}
static int uart_wait_tx_done(int port,int timeout){(void)port;(void)timeout;drains++;return ESP_OK;}
static int usb_serial_jtag_wait_tx_done(int timeout){(void)timeout;drains++;return ESP_OK;}
''' + production + r'''
static int write_bytes(const char *data,size_t count){
  (void)data;assert(count&&g_screenshot.raw&&g_screenshot.b64&&locks==1);
  writes++;
  if(writes==inject_at){
    assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
    // Requested realtime work has priority before sampled audio flags change.
    assert(!screenshot_audio_busy());
    assert(!pet_resources_device_debug_current(&g_screenshot.resources,false));
    pet_resource_usage_t usage;pet_resources_device_usage(&usage);
    assert(usage.leases[PET_RESOURCE_SNAPSHOT]==1&&usage.leases[PET_RESOURCE_DEBUG_BULK]==1);
  }
  return (int)count;
}
static void begin(unsigned inject){
  assert(!g_screenshot.raw&&!realtime.serial&&!locks);
  writes=drains=ticks=0;inject_at=inject;
  assert(pet_resources_device_debug_begin(2,false,&g_screenshot.resources)==PET_RESOURCE_OK);
  g_screenshot.raw=malloc(2);g_screenshot.b64=malloc(8);assert(g_screenshot.raw&&g_screenshot.b64);
}
int main(void){
  assert(screenshot_chunk_checksum(NULL,0)==2166136261u);
  // Ordinary checked transmission still has exactly one drain per full frame.
  for(unsigned route_index=0;route_index<2;route_index++){
    pet_p4_transport_route_t route=route_index?PET_P4_TRANSPORT_USB_SERIAL_JTAG:PET_P4_TRANSPORT_UART;
    begin(0);assert(screenshot_send_parts("header","body",4,"\n",&route));
    assert(writes==3&&drains==1&&!locks);screenshot_release();
    // Audio may reserve in any of the existing physical writes. Cancellation
    // stops at the next boundary, never frees a buffer inside the driver call.
    for(unsigned at=1;at<=3;at++){
      begin(at);assert(!screenshot_send_parts("header","body",4,"\n",&route));
      assert(writes==at&&drains==0&&!locks&&g_screenshot.raw&&g_screenshot.b64);
      pet_resource_usage_t usage;pet_resources_device_usage(&usage);
      assert(usage.leases[PET_RESOURCE_SNAPSHOT]==1&&usage.realtime_reservations==1);
      screenshot_poll();assert(!g_screenshot.raw&&!g_screenshot.b64);
      pet_resources_device_usage(&usage);assert(!usage.leases[PET_RESOURCE_SNAPSHOT]);
      pet_resources_device_realtime_end(&realtime);
    }
    // Normal diagnostic error framing still drains once, without a body write.
    begin(0);screenshot_error("frame","test",0,0,&route);
    assert(writes==2&&drains==1&&!locks);screenshot_release();
  }
  assert(g_screenshot_sequence==0);
  puts("PASS checked screenshot: exact physical ownership and reservation-before-drain cancellation");
}
''')
    (tmp_path / "freertos").mkdir()
    (tmp_path / "freertos/FreeRTOS.h").write_text(
        '#pragma once\n#include <pthread.h>\ntypedef pthread_mutex_t portMUX_TYPE;\n'
        '#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n'
        '#define portENTER_CRITICAL(m) ((void)pthread_mutex_lock(m))\n'
        '#define portEXIT_CRITICAL(m) ((void)pthread_mutex_unlock(m))\n')
    executable = tmp_path / "screenshot-reservation"
    subprocess.run(["cc", *FLAGS, "-I", str(tmp_path), "-I", str(ROOT / "main"),
                    str(source_path), str(ROOT / "main/pet_p4_resources.c"),
                    str(ROOT / "main/pet_p4_resources_device.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)


def test_native_checked_write_rejects_impossible_counts_without_replay(tmp_path):
    source = (ROOT / "main/pet_p4_usb_native.c").read_text()
    production = "static bool native_write_all(" + source.split(
        "static bool native_write_all(", 1)[1].split("static bool native_send_frame(", 1)[0]
    source_path = tmp_path / "native_write.c"
    source_path.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define pdMS_TO_TICKS(ms) (ms)
static const uint8_t data[]={1,2,3,4,5,6,7};
static unsigned mode,calls,flushes,delays;
static size_t accepted;
static uint32_t tud_vendor_write(const uint8_t *bytes,size_t len){
  calls++;assert(bytes==data+accepted&&len==sizeof(data)-accepted);
  if(mode==1)return 0;
  if(mode==2&&calls==2)return UINT32_MAX;
  if(mode==3)return (uint32_t)len+1;
  if(mode==4&&calls%200)return 0;
  size_t n=mode==4?1:(len>3?3:len);
  accepted+=n;return (uint32_t)n;
}
static void tud_vendor_write_flush(void){flushes++;}
static void vTaskDelay(unsigned delay){assert(delay==1);delays++;}
// Compile the same production arithmetic with the P4's 32-bit size_t width.
// No driver logic or control-flow replacement is used in this fixture.
#define size_t uint32_t
''' + production + r'''
#undef size_t
static void reset(unsigned next){mode=next;calls=flushes=delays=0;accepted=0;}
int main(void){
  reset(0);assert(native_write_all(data,0));assert(!calls&&!flushes&&!delays);
  reset(0);assert(native_write_all(data,sizeof(data)));assert(calls==3&&accepted==sizeof(data)&&!delays);
  reset(1);assert(!native_write_all(data,sizeof(data)));assert(calls==200&&flushes==200&&delays==200&&!accepted);
  reset(2);assert(!native_write_all(data,sizeof(data)));assert(calls==2&&accepted==3&&!delays);
  reset(3);assert(!native_write_all(data,sizeof(data)));assert(calls==1&&!accepted&&!delays);
  // Preserve the existing consecutive-idle budget, not a new absolute timeout.
  reset(4);assert(native_write_all(data,sizeof(data)));assert(calls==1400&&accepted==sizeof(data)&&delays==1393);
  puts("PASS native checked writer: partial writes, idle budget, invalid return, no whole-frame replay");
}
''')
    executable = tmp_path / "native-write"
    subprocess.run(["cc", *FLAGS, str(source_path), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=20)
