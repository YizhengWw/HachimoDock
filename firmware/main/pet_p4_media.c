#include "pet_p4_media.h"
#include "cJSON.h"
#include "pet_p4_media_codec.h"
#include "pet_p4_audio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
static SemaphoreHandle_t lock;
static pet_p4_media_view_t view;
static char session[64];
static uint32_t next_seq,offset_ms;
static uint64_t last_request;
static uint8_t *cover;
static size_t cover_bytes;
static char queue_keys[20][180];
typedef struct {uint32_t at_ms;char text[PET_P4_MEDIA_LYRIC_BYTES];} lyric_line_t;
static lyric_line_t *lyrics;
static uint32_t lyric_received,lyric_total;
static pet_p4_send_line_fn send_line;
static void *send_ctx;
static const char *str(const cJSON *v,const char *k) {const cJSON *s=cJSON_GetObjectItemCaseSensitive(v,k);return cJSON_IsString(s)?s->valuestring:"";}
static bool number(const cJSON *v,const char *k,uint32_t max,uint32_t *out) {
  const cJSON *n=cJSON_GetObjectItemCaseSensitive(v,k);
  if (!cJSON_IsNumber(n)||!isfinite(n->valuedouble)||n->valuedouble<0||n->valuedouble>max||floor(n->valuedouble)!=n->valuedouble) return false;
  *out=(uint32_t)n->valuedouble;return true;
}
static void copy(char *out,size_t size,const char *s) {
  size_t n=strlen(s);if(n>=size)n=size-1;
  while(n && ((unsigned char)s[n]&0xc0)==0x80)n--;
  memcpy(out,s,n);out[n]=0;
}
static void emit(const char *topic,cJSON *payload) {
  cJSON *root=cJSON_CreateObject();cJSON_AddStringToObject(root,"topic",topic);cJSON_AddItemToObject(root,"payload",payload);
  char *line=cJSON_PrintUnformatted(root);if(line&&send_line)send_line(line,send_ctx);cJSON_free(line);cJSON_Delete(root);
}
static bool library_valid(const cJSON *p) {
  const cJSON *q=cJSON_GetObjectItemCaseSensitive(p,"queue");
  const cJSON *keys=cJSON_GetObjectItemCaseSensitive(p,"keys");
  if(!cJSON_IsArray(q)||cJSON_GetArraySize(q)>20||!cJSON_IsArray(keys)||cJSON_GetArraySize(keys)!=cJSON_GetArraySize(q))return false;
  for(int i=0;i<cJSON_GetArraySize(q);i++) {
    const cJSON *title=cJSON_GetArrayItem(q,i),*key=cJSON_GetArrayItem(keys,i);
    if(!cJSON_IsString(title)||!cJSON_IsString(key)||!key->valuestring[0]||strlen(key->valuestring)>=180)return false;
  }
  return true;
}
static void apply_library(const cJSON *p) {
  const cJSON *q=cJSON_GetObjectItemCaseSensitive(p,"queue"),*keys=cJSON_GetObjectItemCaseSensitive(p,"keys");
  view.count=cJSON_GetArraySize(q);
  for(int i=0;i<view.count;i++) {copy(view.queue[i],91,cJSON_GetArrayItem(q,i)->valuestring);copy(queue_keys[i],180,cJSON_GetArrayItem(keys,i)->valuestring);}
  if(view.selected>=view.count)view.selected=0;
}
void pet_p4_media_init(void) {
  if(lock)return;lock=xSemaphoreCreateMutex();view.volume=65;
  view.lyric_index=-1;
  copy(view.status,sizeof(view.status),"idle");copy(view.message,sizeof(view.message),"在 PC 搜索歌曲或用实时对话点歌");
}
static void refresh(void) {
  if(!session[0])return;
  if(!pet_p4_audio_music_active()) {
    if(strcmp(view.status,"error")) {copy(view.status,sizeof(view.status),"interrupted");copy(view.message,sizeof(view.message),"语音优先，音乐已暂停");}
    return;
  }
  view.position_ms=offset_ms+pet_p4_audio_stream_played_bytes()/96;
  const char *s=pet_p4_audio_music_paused()?"paused":pet_p4_audio_music_ended()?"ended":pet_p4_audio_stream_playing()?"playing":"buffering";
  copy(view.status,sizeof(view.status),s);
  if(!strcmp(s,"ended"))view.duration_ms=view.position_ms;
}
bool pet_p4_media_handle(const char *topic,const cJSON *p,pet_p4_send_line_fn send,void *ctx) {
  if(strncmp(topic,"media/",6))return false;
  pet_p4_media_init();if(!lock)return false;
  xSemaphoreTake(lock,portMAX_DELAY);send_line=send;send_ctx=ctx;
  bool ok=true;const char *message="";const char *id=str(p,"sessionId");
  uint32_t n=0;bool exact=id[0]&&!strcmp(id,session);
  if(!strcmp(topic,"media/begin")) {
    uint32_t rate,duration,offset,volume;
    if(!id[0]||strlen(id)>=sizeof(session)||strncmp(id,"music-",6)||strcmp(str(p,"format"),"ima-block-v1")
        ||!number(p,"sampleRate",48000,&rate)||rate!=48000
        ||!number(p,"durationMs",3600000,&duration)||!number(p,"offsetMs",3600000,&offset)
        ||!number(p,"volume",100,&volume)||!library_valid(p)) {ok=false;message="音乐格式或参数无效";}
    else if(pet_p4_audio_music_begin(id)!=ESP_OK) {ok=false;message="麦克风或语音正在使用，音乐未启动";}
    else {
      const uint8_t page=view.page==2?2:0;
      memset(&view,0,sizeof(view));view.page=page;view.lyric_index=-1;lyric_received=lyric_total=0;
      view.volume=volume;view.duration_ms=duration;offset_ms=offset;next_seq=0;cover_bytes=0;
      copy(session,sizeof(session),id);copy(view.title,sizeof(view.title),str(p,"title"));copy(view.artist,sizeof(view.artist),str(p,"artist"));
      apply_library(p);
      pet_p4_audio_music_volume(volume);copy(view.status,sizeof(view.status),"buffering");
    }
  } else if(!strcmp(topic,"media/chunk")) {
    static uint8_t encoded[1926],pcm[7680];size_t bytes=0,written=0;const char *data=str(p,"data");
    if(!exact || !pet_p4_audio_music_active()) {ok=false;message="音乐会话已结束";}
    else if(!number(p,"seq",UINT32_MAX,&n)||n!=next_seq||strlen(data)>2568
      ||mbedtls_base64_decode(encoded,sizeof(encoded),&bytes,(const unsigned char*)data,strlen(data))
      ||!pet_p4_media_decode(encoded,bytes,pcm,sizeof(pcm),&written)) {ok=false;message="音乐数据顺序或格式无效";}
    else if(pet_p4_audio_stream_push(pcm,written)!=ESP_OK) {ok=false;message="音乐缓冲区已满";}
    else next_seq++;
  } else if(!strcmp(topic,"media/end")) {
    if(!exact) {ok=false;message="音乐会话不匹配";} else pet_p4_audio_stream_end();
  } else if(!strcmp(topic,"media/stop")) {
    if(!id[0]||exact) {if(pet_p4_audio_music_active())pet_p4_audio_stream_flush();session[0]=0;copy(view.status,sizeof(view.status),"idle");view.position_ms=0;}
  } else if(!strcmp(topic,"media/control")) {
    const char *op=str(p,"operation");
    if(!strcmp(op,"volume")&&number(p,"volume",100,&n)) {view.volume=n;pet_p4_audio_music_volume(n);}
    else if(!pet_p4_audio_music_active()) {ok=false;message="当前没有播放中的音乐";}
    else if(!strcmp(op,"pause"))pet_p4_audio_music_pause(true);
    else if(!strcmp(op,"resume"))pet_p4_audio_music_pause(false);
    else if(!strcmp(op,"toggle"))pet_p4_audio_music_pause(!pet_p4_audio_music_paused());
    else {ok=false;message="播放器操作无效";}
  } else if(!strcmp(topic,"media/cover")) {
    size_t bytes=0;static uint8_t decoded[2048];const char *data=str(p,"data");
    if(!exact || !number(p,"offset",192*192*2,&n)||n!=cover_bytes||strlen(data)>2732
       ||mbedtls_base64_decode(decoded,sizeof(decoded),&bytes,(const unsigned char*)data,strlen(data))
       ||!bytes||cover_bytes+bytes>192*192*2) {ok=false;message="封面数据无效";}
    else {if(!cover)cover=heap_caps_malloc(192*192*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      if(!cover){ok=false;message="封面内存不足";}else{memcpy(cover+cover_bytes,decoded,bytes);cover_bytes+=bytes;view.cover_ready=cover_bytes==192*192*2;}}
  } else if(!strcmp(topic,"media/lyrics")) {
    uint32_t start,total,at=0;
    const cJSON *lines=cJSON_GetObjectItemCaseSensitive(p,"lines");
    int count=cJSON_IsArray(lines)?cJSON_GetArraySize(lines):-1;
    if(!exact||!number(p,"start",PET_P4_MEDIA_LYRIC_MAX,&start)
        ||!number(p,"total",PET_P4_MEDIA_LYRIC_MAX,&total)||count<0||count>4
        ||start!=lyric_received||start+(uint32_t)count>total
        ||(start && total!=lyric_total)||(!count && total)) {ok=false;message="歌词会话或顺序无效";}
    else {
      uint32_t previous=start?lyrics[start-1].at_ms:0;
      for(int i=0;i<count;i++) {
        const cJSON *line=cJSON_GetArrayItem(lines,i),*text=cJSON_GetObjectItemCaseSensitive(line,"text");
        if(!number(line,"atMs",3600000,&at)||at<previous||!cJSON_IsString(text)
            ||strlen(text->valuestring)>=PET_P4_MEDIA_LYRIC_BYTES) {ok=false;message="歌词格式无效";break;}
        previous=at;
      }
      if(ok && count && !lyrics)lyrics=heap_caps_malloc(sizeof(*lyrics)*PET_P4_MEDIA_LYRIC_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      if(ok && count && !lyrics) {ok=false;message="歌词内存不足";}
      if(ok) {
        if(!start){view.lyrics_ready=false;view.lyric_index=-1;lyric_total=total;}
        for(int i=0;i<count;i++) {
          const cJSON *line=cJSON_GetArrayItem(lines,i);number(line,"atMs",3600000,&at);
          lyrics[start+i].at_ms=at;copy(lyrics[start+i].text,sizeof(lyrics[0].text),str(line,"text"));
        }
        lyric_received+=count;view.lyrics_ready=lyric_received==lyric_total;
      }
    }
  } else if(!strcmp(topic,"media/library")) {
    if(!library_valid(p)) {ok=false;message="播放列表格式无效";}
    else apply_library(p);
    if(number(p,"volume",100,&n)) {view.volume=n;pet_p4_audio_music_volume(n);}
  } else if(!strcmp(topic,"media/metadata")) {
    if(!exact || !number(p,"durationMs",3600000,&n)) {ok=false;message="曲目信息已过期";}else view.duration_ms=n;
  } else if(!strcmp(topic,"media/query")) {
    if(id[0]&&!exact) {ok=false;message="音乐会话不匹配";}
  } else {ok=false;message="未知媒体指令";}
  if(ok && strcmp(topic,"media/library"))last_request=esp_timer_get_time()/1000ULL;
  refresh();
  cJSON *reply=cJSON_CreateObject();
  cJSON_AddBoolToObject(reply,"ok",ok);cJSON_AddStringToObject(reply,"requestId",str(p,"requestId"));
  cJSON_AddStringToObject(reply,"sessionId",session);cJSON_AddStringToObject(reply,"state",view.status);
  cJSON_AddStringToObject(reply,"message",ok?view.message:message);
  cJSON_AddNumberToObject(reply,"playedBytes",pet_p4_audio_music_active()?pet_p4_audio_stream_played_bytes():0);
  cJSON_AddNumberToObject(reply,"positionMs",view.position_ms);
  cJSON_AddNumberToObject(reply,"bufferedMs",pet_p4_audio_music_buffered_ms());
  cJSON_AddNumberToObject(reply,"volume",view.volume);
  emit("media/status",reply);xSemaphoreGive(lock);return true;
}
void pet_p4_media_tick(uint64_t now) {
  if(!lock)return;xSemaphoreTake(lock,portMAX_DELAY);
  if(session[0]&&pet_p4_audio_music_active()&&now>last_request+5000) {
    view.position_ms=offset_ms+pet_p4_audio_stream_played_bytes()/96;
    pet_p4_audio_stream_flush();copy(view.status,sizeof(view.status),"error");copy(view.message,sizeof(view.message),"PC 连接已中断，请重新播放");
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_view(pet_p4_media_view_t *out) {
  if(!out)return;memset(out,0,sizeof(*out));if(!lock)return;
  xSemaphoreTake(lock,portMAX_DELAY);refresh();
  int current=-1;
  if(view.lyrics_ready)for(uint32_t i=0;i<lyric_total;i++) {if(lyrics[i].at_ms>view.position_ms)break;current=(int)i;}
  if(current!=view.lyric_index) {view.lyric_index=current;view.lyric_changed_ms=esp_timer_get_time()/1000ULL;}
  *out=view;
  for(int row=0;row<5;row++) {
    int index=current+row-2;
    if(view.lyrics_ready && index>=0 && index<(int)lyric_total)copy(out->lyric_lines[row],sizeof(out->lyric_lines[row]),lyrics[index].text);
    else out->lyric_lines[row][0]=0;
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_blit(uint16_t *frame,int stride,int x,int y) {
  if(!lock||!frame||x<0||y<0||x+192>stride||y+192>480)return;
  xSemaphoreTake(lock,portMAX_DELAY);
  if(view.cover_ready&&cover)for(int row=0;row<192;row++)for(int col=0;col<192;col++) {
    int i=(row*192+col)*2;frame[(y+row)*stride+x+col]=(uint16_t)cover[i]|((uint16_t)cover[i+1]<<8);
  }
  xSemaphoreGive(lock);
}
void pet_p4_media_stop_background(pet_p4_send_line_fn send, void *ctx) {
  pet_p4_media_init();if(!lock)return;
  xSemaphoreTake(lock,portMAX_DELAY);
  // Only flush the music owner: voice and notification audio are unrelated.
  if(pet_p4_audio_music_active())pet_p4_audio_stream_flush();
  session[0]=0;offset_ms=0;view.position_ms=0;view.seeking=false;
  copy(view.status,sizeof(view.status),"idle");
  copy(view.message,sizeof(view.message),"已停止播放");
  send_line=send;send_ctx=ctx;
  // Always notify PC, including when a song is still being prepared there.
  cJSON *payload=cJSON_CreateObject();
  cJSON_AddStringToObject(payload,"operation","stop");
  emit("media/event",payload);
  xSemaphoreGive(lock);
}
bool pet_p4_media_action(const char *action) {
  if(!lock || !action || strncmp(action,"media.",6))return false;
  xSemaphoreTake(lock,portMAX_DELAY);const char *op=action+6;const char *remote=NULL;
  if(!strcmp(op,"lyrics")) {view.page=view.page==2?0:2;view.seeking=false;}
  else if(!strcmp(op,"queue")) {view.page=view.page==1?0:1;view.seeking=false;}
  else if(!strcmp(op,"select")) {if(view.page==1)remote="select";else if(view.page==0)view.seeking=!view.seeking;}
  else if(!strcmp(op,"up")||!strcmp(op,"down")) {
    if(view.page==1 && view.count) view.selected=(view.selected+view.count+(!strcmp(op,"up")?-1:1))%view.count;
    else remote=!strcmp(op,"up")?"volume_up":"volume_down";
  } else if(!strcmp(op,"previous"))remote=view.seeking?"seek_back":"previous";
  else if(!strcmp(op,"next"))remote=view.seeking?"seek_forward":"next";
  else if(!strcmp(op,"toggle"))remote="toggle";
  else {xSemaphoreGive(lock);return false;}
  if(remote) {cJSON *payload=cJSON_CreateObject();cJSON_AddStringToObject(payload,"operation",remote);if(view.selected<view.count)cJSON_AddStringToObject(payload,"key",queue_keys[view.selected]);emit("media/event",payload);}
  xSemaphoreGive(lock);return true;
}
