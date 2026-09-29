"""Execute the real media protocol and navigation with simulated hardware, not real audio."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_media_sessions_queue_controls_and_watchdog(tmp_path):
    cjson = Path.home() / ".platformio/packages/framework-espidf/components/json/cJSON"
    stubs = {
        "esp_err.h": "typedef int esp_err_t;\n#define ESP_OK 0\n",
        "esp_heap_caps.h": "#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n#define heap_caps_malloc(n,c) malloc(n)\n",
        "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "freertos/FreeRTOS.h": "#define portMAX_DELAY 0\n",
        "freertos/semphr.h": "typedef int SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(a,b) ((void)0)\n#define xSemaphoreGive(a) ((void)0)\n",
        "mbedtls/base64.h": "#include <stddef.h>\nint mbedtls_base64_decode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);\n",
    }
    for name, source in stubs.items():
        file = tmp_path / name; file.parent.mkdir(parents=True, exist_ok=True); file.write_text(source)
    harness = tmp_path / "runtime.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "pet_p4_media.h"
#include "pet_p4_audio.h"
static bool active, paused, ended, busy;
static unsigned int flushes;
static uint32_t played;
static int64_t clock_us=1000000;
static cJSON *reply;
int64_t esp_timer_get_time(void){return clock_us;}
bool pet_p4_audio_music_active(void){return active;}
bool pet_p4_audio_music_paused(void){return paused;}
bool pet_p4_audio_music_ended(void){return ended;}
bool pet_p4_audio_stream_playing(void){return active&&!paused&&!ended;}
uint32_t pet_p4_audio_stream_played_bytes(void){return played;}
unsigned int pet_p4_audio_music_buffered_ms(void){return 200;}
esp_err_t pet_p4_audio_music_begin(const char *id){(void)id;if(busy)return -1;active=true;paused=false;ended=false;played=0;return 0;}
esp_err_t pet_p4_audio_stream_push(const uint8_t *p,size_t n){(void)p;played+=(uint32_t)n;return 0;}
esp_err_t pet_p4_audio_stream_end(void){ended=true;return 0;}
void pet_p4_audio_stream_flush(void){active=false;flushes++;}
void pet_p4_audio_music_pause(bool value){paused=value;}
void pet_p4_audio_music_volume(unsigned int value){(void)value;}
int mbedtls_base64_decode(unsigned char *out,size_t cap,size_t *n,const unsigned char *in,size_t len){
  if(cap<6||len!=8||memcmp(in,"AAAoAAEA",8))return -1;
  const unsigned char data[]={0,0,40,0,1,0};memcpy(out,data,6);*n=6;return 0;
}
static void sent(const char *line,void *ctx){(void)ctx;cJSON_Delete(reply);reply=cJSON_Parse(line);assert(reply);}
static cJSON *payload(void){return cJSON_GetObjectItemCaseSensitive(reply,"payload");}
static bool call(const char *topic,const char *json){cJSON *p=cJSON_Parse(json);assert(p);assert(pet_p4_media_handle(topic,p,sent,NULL));cJSON_Delete(p);return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload(),"ok"));}
static const char *field(const char *name){return cJSON_GetObjectItemCaseSensitive(payload(),name)->valuestring;}
int main(void){
  pet_p4_media_init();
  assert(call("media/library","{\"queue\":[\"重复标题\",\"重复标题\"],\"keys\":[\"local:1\",\"local:2\"],\"volume\":25}"));
  assert(pet_p4_media_action("media.queue"));assert(pet_p4_media_action("media.down"));assert(pet_p4_media_action("media.select"));assert(!strcmp(field("key"),"local:2"));
  pet_p4_media_view_t view;pet_p4_media_view(&view);assert(view.selected==1&&view.page==1);
  assert(!call("media/library","{\"queue\":[\"bad\",4],\"keys\":[\"local:3\",\"local:4\"]}"));
  pet_p4_media_view(&view);assert(view.count==2&&view.selected==1);
  const char *begin="{\"sessionId\":\"music-new\",\"sampleRate\":48000,\"format\":\"ima-block-v1\",\"durationMs\":20000,\"offsetMs\":0,\"volume\":25,\"queue\":[\"歌曲\"],\"keys\":[\"local:1\"]}";
  busy=true;assert(!call("media/begin",begin));assert(!active);busy=false;assert(call("media/begin",begin));
  assert(!call("media/lyrics","{\"sessionId\":\"music-old\",\"start\":0,\"total\":0,\"lines\":[]}"));
  assert(!call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":2,\"lines\":[{\"atMs\":5000,\"text\":\"bad\"},{\"atMs\":2000,\"text\":\"bad\"}]}"));
  assert(call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":2,\"lines\":[{\"atMs\":1000,\"text\":\"测试第一句\"}]}"));
  pet_p4_media_view(&view);assert(!view.lyrics_ready);
  assert(!call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":2,\"lines\":[{\"atMs\":2000,\"text\":\"重复块\"}]}"));
  assert(call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":1,\"total\":2,\"lines\":[{\"atMs\":2000,\"text\":\"测试第二句\"}]}"));
  assert(pet_p4_media_action("media.lyrics"));played=1500*96;pet_p4_media_view(&view);
  assert(view.page==2&&view.lyric_index==0&&!strcmp(view.lyric_lines[2],"测试第一句"));
  played=2500*96;pet_p4_media_view(&view);assert(view.lyric_index==1&&!strcmp(view.lyric_lines[2],"测试第二句"));
  paused=true;pet_p4_media_view(&view);assert(view.lyric_index==1);paused=false;
  played=500*96;pet_p4_media_view(&view);assert(view.lyric_index==-1);
  assert(pet_p4_media_action("media.lyrics"));pet_p4_media_view(&view);assert(view.page==0);
  assert(pet_p4_media_action("media.lyrics"));assert(call("media/begin",begin));pet_p4_media_view(&view);
  assert(view.page==2&&!view.lyrics_ready&&view.lyric_index==-1&&!view.lyric_lines[2][0]);
  assert(call("media/lyrics","{\"sessionId\":\"music-new\",\"start\":0,\"total\":0,\"lines\":[]}"));
  pet_p4_media_view(&view);assert(view.lyrics_ready&&!view.lyric_lines[2][0]);
  assert(!call("media/chunk","{\"sessionId\":\"music-old\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));
  assert(call("media/chunk","{\"sessionId\":\"music-new\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));
  assert(!call("media/chunk","{\"sessionId\":\"music-new\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));assert(played==2);
  assert(call("media/stop","{\"sessionId\":\"music-old\"}"));assert(active);
  assert(call("media/control","{\"operation\":\"pause\"}"));assert(paused&&!strcmp(field("state"),"paused"));
  assert(call("media/control","{\"operation\":\"resume\"}"));assert(!paused);
  assert(!call("media/end","{\"sessionId\":\"music-old\"}"));assert(!ended);
  assert(call("media/end","{\"sessionId\":\"music-new\"}"));assert(ended&&!strcmp(field("state"),"ended"));
  // Back on the pet home page stops locally, preserves the queue and notifies PC.
  assert(call("media/begin",begin));
  assert(pet_p4_media_action("media.lyrics"));assert(active);
  pet_p4_media_stop_background(sent,NULL);assert(!active);
  assert(!strcmp(field("operation"),"stop"));
  pet_p4_media_view(&view);assert(!strcmp(view.status,"idle")&&view.position_ms==0&&view.count==1);
  pet_p4_media_stop_background(sent,NULL);assert(!active);
  assert(!call("media/chunk","{\"sessionId\":\"music-new\",\"seq\":0,\"data\":\"AAAoAAEA\"}"));
  assert(!call("media/query","{\"sessionId\":\"music-new\"}"));
  // Pending preparation also emits a cancellation, without touching voice audio.
  unsigned int music_flushes=flushes;
  pet_p4_media_stop_background(sent,NULL);
  assert(flushes==music_flushes);
  assert(!strcmp(field("operation"),"stop"));
  assert(call("media/begin",begin));
  assert(call("media/control","{\"operation\":\"pause\"}"));
  pet_p4_media_stop_background(sent,NULL);assert(!active);
  assert(call("media/begin",begin));clock_us=9000000;
  assert(call("media/library","{\"queue\":[],\"keys\":[]}"));pet_p4_media_tick(9000);assert(!active);
  pet_p4_media_view(&view);assert(!strcmp(view.status,"error"));
  assert(!pet_p4_media_action("media.shell"));
  cJSON_Delete(reply);puts("PASS media stale sessions, seq, queue identities, atomic validation, controls, lease expiry");
}
''')
    exe = tmp_path / "media-test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-include", str(cjson / "cJSON.h"), "-I", str(tmp_path), "-I", str(ROOT / "main"), "-I", str(cjson), str(ROOT / "main/pet_p4_media.c"), str(ROOT / "main/pet_p4_media_codec.c"), str(cjson / "cJSON.c"), str(harness), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
