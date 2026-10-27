"""Exercise the actual package renderer with ASan/UBSan, not just its JSON parser."""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_media_snapshots_are_renderer_owned_not_nested_stack_copies():
    source = (ROOT / 'firmware/main/pet_p4_renderer.c').read_text()
    assert 'static EXT_RAM_BSS_ATTR pet_p4_miniapp_view_t g_miniapp_view_snapshot;' in source
    assert 'static EXT_RAM_BSS_ATTR pet_p4_media_view_t g_media_view_snapshot;' in source
    render = source.split('static bool render_package_media', 1)[1].split('/* Lossless instrument layers:', 1)[0]
    assert 'pet_p4_media_view_t m;' not in render
    assert render.count('pet_p4_media_view(m)') == 1
    miniapp = source.split('static void render_miniapp_page', 1)[1]
    assert 'pet_p4_miniapp_view_t snapshot' not in miniapp
    build = (ROOT / 'firmware/main/CMakeLists.txt').read_text()
    assert '-fstack-usage' in build
    image = (ROOT / 'firmware/scripts/platformio_chip_images.py').read_text()
    assert 'sum(frames.get(name, 0) for name in chain) > 4096' in image
    assert 'g_surface_background_valid' in source
    assert 'if(a==255){out[dx]=colors[index];continue;}' in source
    assert 'int32_t sx=row_x,sy=row_y' in source
    assert 'pet_p4_miniapp_media_surface_animated()' in (ROOT/'firmware/main/pet_p4_main.c').read_text()


def test_actual_radio_renderer_empty_full_queue_images_rotation_and_bad_resource(tmp_path):
    source = (ROOT / 'firmware/main/pet_p4_renderer.c').read_text()
    renderer = 'static EXT_RAM_BSS_ATTR pet_p4_miniapp_sprite_pack_t g_surface_images;' + source.split(
        'static EXT_RAM_BSS_ATTR pet_p4_miniapp_sprite_pack_t g_surface_images;', 1
    )[1].split('static void render_media_page', 1)[0]
    # Application typography has its own harness; this exercises the actual
    # shared image cache and media renderer without compiling unrelated LVGL code.
    renderer=renderer.split('static void draw_app_panel',1)[0]+'static bool render_package_media'+renderer.split('static bool render_package_media',1)[1]
    (tmp_path / 'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n')
    harness = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_miniapp.h"
#include "pet_p4_media.h"
#include "pet_p4_image.h"
#define EXT_RAM_BSS_ATTR
static uint16_t g_framebuffer[640*480];
static pet_surface_t fixture;
static pet_p4_miniapp_sprite_pack_t packed;
static unsigned revision=1;
static unsigned full_background_clears;
typedef int JRESULT;
static unsigned char *g_jpeg_input;
static unsigned g_jpeg_input_capacity;
static uint16_t *g_jpeg_output;
static bool decode_jpeg_frame_hardware(const void*p,size_t n,int*w,int*h,int*s,esp_err_t*e){assert(!"unexpected JPEG in palette rotation test");return false;}
static bool decode_jpeg_frame_tjpgd(const void*p,size_t n,int*w,int*h,JRESULT*e){assert(!"unexpected JPEG in palette rotation test");return false;}
bool pet_p4_miniapp_media_config(char source[48],pet_surface_t *surface){
  strcpy(source,"pc.city-radio");*surface=fixture;return true;
}
bool pet_p4_miniapp_get_surface_images(pet_p4_miniapp_sprite_pack_t *out,uint32_t *seen){
  if(*seen==revision)return false;*seen=revision;*out=packed;return true;
}
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void fill_rect(int x,int y,int w,int h,uint16_t c){
  assert(x>=0&&y>=0&&w>=0&&h>=0&&x+w<=640&&y+h<=480);
  if(x==0&&y==0&&w==640&&h==480)full_background_clears++;
  for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)g_framebuffer[j*640+i]=c;
}
static void fill_round_rect_outline(int x,int y,int w,int h,int r,uint16_t c,uint16_t o){
  (void)r;(void)o;fill_rect(x,y,w,h,c);
}
static void fill_ellipse(int x,int y,int rx,int ry,uint16_t c){fill_rect(x-rx,y-ry,rx*2,ry*2,c);}
static void draw_text_line(const char*t,int x,int y,int w,uint16_t c,int s,bool e){
  (void)t;(void)c;(void)e;assert(x>=0&&x+w<=640&&y>=0&&y+32*s<=480);
}
static void load_fixture(const char *path){
  FILE *f=fopen(path,"rb");assert(f);char buf[8192];size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]=0;fclose(f);
  cJSON *j=cJSON_Parse(buf);assert(j);assert(pet_surface_parse(j,&fixture));cJSON_Delete(j);
  memset(&packed,0,sizeof(packed));packed.count=fixture.image_count;
  for(unsigned i=0;i<packed.count;i++){
    pet_p4_miniapp_sprite_t *im=&packed.items[i];
    im->frame_width=fixture.image_w[i];im->frame_height=fixture.image_h[i];im->frames=1;
    im->data_offset=packed.data_length;uint8_t *p=packed.pixels+im->data_offset;
    memcpy(p,"P4I1",4);p[4]=im->frame_width;p[5]=im->frame_height;p[6]=1;
    p[12]=220;p[13]=170;p[14]=80;p[15]=255;
    size_t at=1036,left=(size_t)p[4]*p[5];
    while(left){unsigned count=left>128?128:left;p[at++]=128|(count-1);p[at++]=0;left-=count;}
    for(int b=0;b<4;b++)p[8+b]=(at>>(b*8))&255;
    im->data_length=at;packed.data_length+=at;
  }
  revision++;
}
'''
    main = r'''
static void reference_image(const pet_surface_node_t *n){
  const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[n->image];
  const uint8_t *palette=g_surface_images.pixels+im->data_offset+12;
  const uint8_t *indices=g_surface_indices+g_surface_offsets[n->image];
  float angle=n->rotate_ms?(float)(g_surface_play_ms%n->rotate_ms)*6.2831853f/n->rotate_ms:0;
  float co=cosf(angle),si=sinf(angle);int iw=im->frame_width,ih=im->frame_height;
  for(int dy=0;dy<n->h;dy++)for(int dx=0;dx<n->w;dx++){
    float px=(dx+.5f)/n->w-.5f,py=(dy+.5f)/n->h-.5f;
    float sx=(co*px+si*py+.5f)*iw,sy=(-si*px+co*py+.5f)*ih;
    if(sx<0||sy<0||sx>=iw||sy>=ih)continue;
    const uint8_t *p=palette+4*indices[(int)sy*iw+(int)sx];unsigned a=p[3];if(!a)continue;
    size_t at=(n->y+dy)*640+n->x+dx;uint16_t bg=g_framebuffer[at];
    unsigned r=((bg>>11)&31)*255/31,g=((bg>>5)&63)*255/63,b=(bg&31)*255/31;
    g_framebuffer[at]=rgb565((p[0]*a+r*(255-a))/255,(p[1]*a+g*(255-a))/255,(p[2]*a+b*(255-a))/255);
  }
}
int main(int argc,char **argv){
  assert(argc==3);static pet_p4_media_view_t m;
  for(int layout=1;layout<argc;layout++){
    load_fixture(argv[layout]);
    for(int count=0;count<=20;count++)for(int page=0;page<=1;page++){
      memset(&m,0,sizeof(m));strcpy(m.source,"pc.city-radio");m.count=count;m.page=page;m.volume=50;
      for(int i=0;i<count;i++)snprintf(m.queue[i],sizeof(m.queue[i]),"Radio %d",i);
      for(int select=0;select<(count?count:1);select++){
        m.selected=select;assert(render_package_media(1000,&m));
      }
    }
  }
  m.page=0;strcpy(m.status,"playing");
  unsigned clears=full_background_clears;
  render_package_media(1010,&m);uint64_t initial=g_surface_play_ms;
  assert(full_background_clears==clears);
  render_package_media(1060,&m);assert(g_surface_play_ms==initial+50);
  strcpy(m.status,"idle");render_package_media(1110,&m);assert(g_surface_play_ms==initial+50);
  strcpy(m.status,"buffering");render_package_media(1160,&m);assert(g_surface_play_ms==initial+50);
  strcpy(m.status,"playing");render_package_media(1210,&m);render_package_media(1260,&m);assert(g_surface_play_ms==initial+100);
  assert(g_surface_background_valid);
  // Compare fixed-point sampling against the prior float transform, including
  // transparency. At most 1% may choose an adjacent texel at rounding borders.
  static uint16_t expected[640*480];
  for(unsigned id=0;id<g_surface_images.count;id++){
    const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[id];
    uint8_t *p=g_surface_images.pixels+im->data_offset+12;
    for(int i=0;i<256;i++){
      p[4*i]=i;p[4*i+1]=255-i;p[4*i+2]=i/2;p[4*i+3]=(i%4)*85;
      g_surface_colors[id][i]=rgb565(p[4*i],p[4*i+1],p[4*i+2]);g_surface_alpha[id][i]=p[4*i+3];
    }
    for(int y=0;y<im->frame_height;y++)for(int x=0;x<im->frame_width;x++)
      g_surface_indices[g_surface_offsets[id]+y*im->frame_width+x]=(x*3+y*5)&255;
  }
  for(int node=0;node<fixture.count;node++)if(fixture.nodes[node].type==6){
    const pet_surface_node_t *n=&fixture.nodes[node];
    for(int t=0;t<20000;t+=2111){
      g_surface_play_ms=t;
      for(int p=0;p<640*480;p++)g_framebuffer[p]=0x1357;
      reference_image(n);memcpy(expected,g_framebuffer,sizeof(expected));
      for(int p=0;p<640*480;p++)g_framebuffer[p]=0x1357;
      draw_surface_image(n);unsigned different=0;
      for(int y=0;y<480;y++)for(int x=0;x<640;x++){
        if(x<n->x||x>=n->x+n->w||y<n->y||y>=n->y+n->h)assert(g_framebuffer[y*640+x]==0x1357);
        if(expected[y*640+x]!=g_framebuffer[y*640+x])different++;
      }
      assert(different<=(unsigned)(n->w*n->h/100));
    }
  }
  // A truncated/corrupt image safely disappears, while the rest of the UI renders.
  packed.items[0].data_length=12;revision++;render_package_media(1310,&m);assert(g_surface_images.count==0);
  assert(g_framebuffer[0]==fixture.background);
  return 0;
}
'''
    code = tmp_path / 'renderer.c'
    code.write_text(harness + renderer + main)
    cjson = ROOT / 'firmware/tests/vendor/cjson'
    binary = tmp_path / 'renderer'
    subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-g', '-I', str(tmp_path),
                    '-I', str(ROOT/'firmware/main'), '-I', str(cjson), str(code),
                    str(ROOT/'firmware/main/pet_p4_surface.c'), str(cjson/'cJSON.c'), '-lm', '-o', str(binary)], check=True)
    new = json.loads((ROOT/'pc/builtin-clawpkgs/city-radio/runtime/widget.json').read_text())['surface']
    old = {'background':65535,'elements':[
        {'type':'text','x':24,'y':24,'w':500,'h':64,'color':1,'scale':2,'bind':'title'},
        {'type':'list','x':24,'y':100,'w':590,'h':288,'color':1,'page':1},
        {'type':'transport','x':280,'y':400,'w':60,'h':60,'color':1,'page':0}]}
    paths = []
    for name, layout in [('legacy',old),('vinyl',new)]:
        file = tmp_path / f'{name}.json';file.write_text(json.dumps(layout));paths.append(str(file))
    subprocess.run([str(binary), *paths], check=True)
