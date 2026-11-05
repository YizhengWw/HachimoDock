/* P4's RGB565 framebuffer + LVGL fonts + TJpgDec approach, round-screen backend. */
#include "stopwatch_renderer.h"
#include "board_stopwatch.h"
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "libs/tjpgd/tjpgd.h"

extern const lv_font_t pet_p4_font_cn_16;
extern const uint8_t _binary_stopwatch_pet_bin_start[], _binary_stopwatch_pet_bin_end[];
static uint16_t *framebuffer;
static uint8_t *jpeg_work;
static const uint8_t *jpeg_data;
static size_t jpeg_size, jpeg_position;
static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return u16(p) | ((uint32_t)u16(p+2) << 16); }
static uint16_t rgb(unsigned r, unsigned g, unsigned b) {
  return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}
static void pixel(int x, int y, uint16_t color) {
  if (x >= 0 && x < STOPWATCH_WIDTH && y >= 0 && y < STOPWATCH_HEIGHT)
    framebuffer[y*STOPWATCH_WIDTH+x] = color;
}
static size_t jpeg_input(JDEC *decoder, uint8_t *out, size_t length) {
  (void)decoder;
  if (length > jpeg_size-jpeg_position) length = jpeg_size-jpeg_position;
  if (out) memcpy(out,jpeg_data+jpeg_position,length);
  jpeg_position += length;
  return length;
}
static int jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect) {
  (void)decoder;
  const uint8_t *bytes = bitmap;
  const uint16_t *words = bitmap;
  unsigned n = 0;
  for (int y = rect->top; y <= rect->bottom; ++y)
    for (int x = rect->left; x <= rect->right; ++x,++n) {
#if JD_FORMAT == 0
      pixel(x+113,y+65,rgb(bytes[n*3],bytes[n*3+1],bytes[n*3+2]));
#else
      pixel(x+113,y+65,words[n]);
#endif
    }
  (void)bytes; (void)words;
  return 1;
}
static void pet_frame(const char *life, uint64_t now) {
  const uint8_t *pack = _binary_stopwatch_pet_bin_start;
  size_t size = _binary_stopwatch_pet_bin_end-pack;
  if (size < 16 || memcmp(pack,"HSP1",4) || u16(pack+4) != 240 || u16(pack+6) != 240) return;
  unsigned families = u16(pack+10), frames = u32(pack+12), fps = u16(pack+8);
  size_t table = 16 + families*28;
  if (!fps || families > 16 || frames > 256 || table+frames*8 > size) return;
  const uint8_t *entry = pack+16;
  for (unsigned i = 0; i < families; ++i)
    if (!strncmp((const char *)(pack+16+i*28),life,24)) { entry = pack+16+i*28; break; }
  unsigned first = u16(entry+24), count = u16(entry+26);
  if (!count || first+count > frames) return;
  unsigned frame = first + (now*fps/1000)%count;
  size_t offset = u32(pack+table+frame*8), length = u32(pack+table+frame*8+4);
  if (offset > size || length > size-offset) return;
  jpeg_data = pack+offset; jpeg_size = length; jpeg_position = 0;
  JDEC decoder;
  if (jd_prepare(&decoder,jpeg_input,jpeg_work,4096,NULL) == JDR_OK &&
      decoder.width == 240 && decoder.height == 240)
    (void)jd_decomp(&decoder,jpeg_output,0);
}
static uint32_t codepoint(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  unsigned n = s[0] < 128 ? 1 : (s[0] & 224) == 192 ? 2 : (s[0] & 240) == 224 ? 3 : 4;
  uint32_t value = s[0] & (n == 1 ? 127 : (1u << (7-n))-1);
  for (unsigned i = 1; i < n; ++i) {
    if (!s[i] || (s[i] & 192) != 128) { *p += 1; return '?'; }
    value = (value << 6) | (s[i] & 63);
  }
  *p += n; return value;
}
static void text(const char *value, int x, int y, int right, uint16_t color) {
  while (*value) {
    uint32_t cp = codepoint(&value);
    lv_font_glyph_dsc_t glyph = {0};
    if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16,&glyph,cp,0)) continue;
    if (x+(int)glyph.adv_w > right) break;
    glyph.req_raw_bitmap = 1;
    const uint8_t *bits = glyph.resolved_font->get_glyph_bitmap(&glyph,NULL);
    if (bits) {
      for (unsigned yy = 0; yy < glyph.box_h; ++yy)
        for (unsigned xx = 0; xx < glyph.box_w; ++xx) {
          unsigned at = yy*glyph.box_w+xx;
          if ((bits[at/8] >> (7-at%8)) & 1)
            pixel(x+glyph.ofs_x+xx,y+pet_p4_font_cn_16.line_height-pet_p4_font_cn_16.base_line-glyph.box_h-glyph.ofs_y+yy,color);
        }
    }
    x += glyph.adv_w;
    lv_font_glyph_release_draw_data(&glyph);
  }
}
esp_err_t stopwatch_renderer_init(void) {
  framebuffer = heap_caps_calloc(STOPWATCH_WIDTH*STOPWATCH_HEIGHT,2,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  jpeg_work = heap_caps_malloc(4096,MALLOC_CAP_8BIT);
  return framebuffer && jpeg_work ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t stopwatch_renderer_draw(const pet_device_sessions_t *m, uint64_t now, int battery) {
  if (!framebuffer) return ESP_ERR_INVALID_STATE;
  memset(framebuffer,0,STOPWATCH_WIDTH*STOPWATCH_HEIGHT*2);
  bool online = m->host_seen && (now < m->last_host_ms ||
    now-m->last_host_ms < PET_DEVICE_SESSION_TIMEOUT_MS);
  const char *life = pet_device_sessions_lifecycle(m);
  pet_frame(online ? life : "idle",now);
  uint16_t white = rgb(240,240,240), muted = rgb(150,170,170);
  uint16_t tone = !strcmp(life,"error") ? rgb(255,90,90) :
    !strcmp(life,"waiting_user") ? rgb(255,190,60) : rgb(100,220,170);
  char info[64];
  snprintf(info,sizeof(info),"USB %s  %d mV",online ? "online" : "offline",battery);
  text(info,132,38,360,muted);
  const pet_device_session_t *card = pet_device_sessions_selected(m);
  const char *label = !online ? "等待电脑连接" : !strcmp(life,"working") ? "工作中" :
    !strcmp(life,"waiting_user") ? "等待操作" : !strcmp(life,"done") ? "已完成" :
    !strcmp(life,"error") ? "出错了" : !strcmp(life,"stopped") ? "已停止" : "休息中";
  text(label,196,308,370,tone);
  text(card ? card->title : m->title,85,343,381,white);
  text(card ? card->content : m->content,85,368,381,muted);
  snprintf(info,sizeof(info),"%u / %u",m->count ? m->selected+1 : 0,m->count);
  text(info,211,405,300,muted);
  return stopwatch_board_present(framebuffer);
}
esp_err_t stopwatch_renderer_diagnostic(unsigned pattern, int tx, int ty) {
  if (!framebuffer) return ESP_ERR_INVALID_STATE;
  for (int y = 0; y < STOPWATCH_HEIGHT; ++y) {
    for (int x = 0; x < STOPWATCH_WIDTH; ++x) {
      uint16_t color = 0;
      if (pattern == 1) color = x < 155 ? rgb(255,0,0) : x < 311 ? rgb(0,255,0) : rgb(0,0,255);
      else if (x % 50 == 0 || y % 50 == 0) color = rgb(45,65,65);
      int dx = x-233, dy = y-233, radius = dx*dx+dy*dy;
      if (radius >= 229*229 && radius <= 231*231) color = 0xffff;
      pixel(x,y,color);
    }
  }
  if (pattern == 1) {
    text("RED",90,215,150,0xffff);
    text("GREEN",205,215,300,0xffff);
    text("BLUE",350,215,420,0xffff);
  } else {
    if (tx >= 0 && ty >= 0) {
      for (int d = -12; d <= 12; ++d) { pixel(tx+d,ty,0xffe0); pixel(tx,ty+d,0xffe0); }
    }
    char label[64];
    snprintf(label,sizeof(label),"Touch x=%d y=%d",tx,ty);
    text(label,130,350,400,0xffff);
    text("触摸圆屏，观察十字是否跟手",125,385,360,0xffff);
  }
  return stopwatch_board_present(framebuffer);
}
