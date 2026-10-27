#pragma once
#include "pet_p4_protocol.h"
#define PET_P4_MEDIA_COVER_SIZE 192
#define PET_P4_MEDIA_QUEUE_MAX 128
#define PET_P4_MEDIA_LYRIC_MAX 256
#define PET_P4_MEDIA_LYRIC_BYTES 181
typedef struct {
  char title[121], artist[91], status[20], message[100];
  uint32_t position_ms, duration_ms;
  uint32_t queue_revision;
  uint8_t volume, page, selected, count;
  bool seeking, cover_ready;
  char source[48];
  uint8_t level;
  char queue[PET_P4_MEDIA_QUEUE_MAX][91];
  bool lyrics_ready;
  int lyric_index;
  uint64_t lyric_changed_ms;
  char lyric_lines[5][PET_P4_MEDIA_LYRIC_BYTES];
} pet_p4_media_view_t;
void pet_p4_media_init(void);
void pet_p4_media_transport(pet_p4_send_line_fn send,void *ctx);
bool pet_p4_media_handle(const char *topic,const cJSON *payload,pet_p4_send_line_fn send,void *ctx);
bool pet_p4_media_action(const char *action);
bool pet_p4_media_live_action(const char *source,const char *action);
bool pet_p4_media_touch(const char *source,int index,int volume,uint32_t revision,uint64_t now_ms);
void pet_p4_media_stop_background(pet_p4_send_line_fn send, void *ctx);
void pet_p4_media_tick(uint64_t now_ms);
void pet_p4_media_visibility(const char *source, bool visible, uint64_t now_ms);
void pet_p4_media_view(pet_p4_media_view_t *out);
void pet_p4_media_blit(uint16_t *frame,int stride,int x,int y);
