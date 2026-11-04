"""Host-only extraction of the production big renderer, real codec and tile selector."""
import subprocess
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def big_renderer(tmp_path_factory):
    tmp = tmp_path_factory.mktemp('big-renderer')
    fragment = (ROOT/'main/pet_p4_renderer_ext.inc').read_text().split('\n#if PET_P4_CLIP\n', 1)[0]
    selector = (ROOT/'main/pet_p4_component_ext.c').read_text().split('unsigned pet_cext_big_visible_tiles(', 1)[1].split('\nint pet_cext_clip_target', 1)[0]
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_component_ext.h"
#include "pet_p4_component_media_clients.h"
#include "pet_p4_image.h"
#define PET_P4_BIG_IMAGE 1
#define PET_P4_CLIP 0
#define EXT_RAM_BSS_ATTR
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t framebuffer[640*480], *g_framebuffer=framebuffer;
typedef struct {int big,big_x,big_y,w,h,x,y;} pet_surface_node_t;
static unsigned attempts,fail_read,fail_decode,sample_alpha;
static bool partial;
static uint32_t store_generation=1;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static uint16_t blend_rgb565(uint16_t c,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
 unsigned dr=((c>>11)&31)*255/31,dg=((c>>5)&63)*255/63,db=(c&31)*255/31;
 return rgb565((r*a+dr*(255-a))/255,(g*a+dg*(255-a))/255,(b*a+db*(255-a))/255);
}
static bool app_clip_contains(int x,int y){return x>=0&&y>=0&&x<640&&y<480;}
static uint16_t color(unsigned big,unsigned tile){return rgb565(8*(tile+1),80+big*80,store_generation*32);}
bool pet_cmedia_big_meta_try_at(unsigned big,uint32_t gen,pet_cext_big_t *m){
 if(big>=2||gen!=store_generation)return false;
 *m=(pet_cext_big_t){.w=960,.h=960,.tile_w=240,.tile_h=240,.cols=4,.rows=4};return true;
}
/* File boundary double: counts every attempt, and reads a real bounded file.
 * Production renderer + actual P4I decoder + actual selector remain unchanged. */
bool pet_cmedia_big_tile_try_at(unsigned big,unsigned tile,uint32_t gen,uint8_t *out,size_t cap,size_t *bytes){
 attempts++;*bytes=0;if(fail_read||gen!=store_generation)return false;
 uint8_t p[4096]={ 'P','4','I','1',240,240,1,0 };size_t n=PET_IMAGE_HEADER;
 p[12]=8*(tile+1);p[13]=80+big*80;p[14]=store_generation*32;p[15]=255;
 if(partial){p[12]=9;p[13]=101;p[14]=37;p[15]=sample_alpha;}
 for(unsigned left=240*240;left;){unsigned len=left>128?128:left;p[n++]=128|(len-1);p[n++]=0;left-=len;}
 p[8]=n;p[9]=n>>8;p[10]=n>>16;p[11]=n>>24;if(fail_decode)p[0]='X';
 assert(n<=cap);FILE *f=tmpfile();assert(f);assert(fwrite(p,1,n,f)==n);rewind(f);
 assert(fread(out,1,n,f)==n);fclose(f);*bytes=n;return true;
}
'''
    harness += '\nunsigned pet_cext_big_visible_tiles(' + selector + '\n' + fragment
    harness += r'''
static pet_surface_node_t node(unsigned big,int x,int y){return (pet_surface_node_t){.big=big,.big_x=x,.big_y=y,.w=640,.h=480};}
static void complete(const pet_surface_node_t *n){
 memset(framebuffer,0,sizeof(framebuffer));renderer_ext_draw_big(n);
 int ox=n->big_x<0?0:n->big_x>320?320:n->big_x;
 int oy=n->big_y<0?0:n->big_y>480?480:n->big_y;
 for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++){
  unsigned tile=((oy+y)/240)*4+(ox+x)/240;
  assert(framebuffer[y*640+x]==color(n->big,tile));
 }
}
static void load_both(void){
 unsigned missing=0;
 for(unsigned i=0;i<32;i++)if(!g_big_slots[i].valid||g_big_slots[i].generation!=store_generation)missing++;
 for(unsigned frame=0;frame<missing;frame++){
  renderer_ext_media_snapshot(store_generation);unsigned before=attempts;
  /* Four nodes, both sources, every tile; cached hits must not consume budget. */
  for(unsigned pass=0;pass<2;pass++)for(unsigned b=0;b<2;b++)for(unsigned t=0;t<16;t++)renderer_big_slot(b,t,store_generation);
  assert(attempts==before+1);
 }
 for(unsigned b=0;b<2;b++)for(unsigned t=0;t<16;t++)assert(renderer_big_slot(b,t,store_generation));
}
int main(int argc,char **argv){assert(argc==2);int mode=atoi(argv[1]);
 if(mode==0){
  pet_cext_big_t m;assert(pet_cmedia_big_meta_try_at(0,1,&m));uint8_t tiles[16];
  assert(pet_cext_big_visible_tiles(&m,239,239,640,480,tiles)==12);
  pet_surface_node_t a=node(0,239,239),b=node(1,239,239);
  /* One reset for all nodes; cold progress survives successive frames. */
  for(unsigned f=0;f<24;f++){renderer_ext_media_snapshot(1);unsigned before=attempts;
   renderer_ext_draw_big(&a);renderer_ext_draw_big(&b);renderer_ext_draw_big(&a);
   assert(attempts==before+1);
  }
  unsigned before=attempts;complete(&a);complete(&b);assert(attempts==before);
 }else if(mode==1){
  load_both();assert(RENDERER_BIG_SLOTS==32);unsigned before=attempts;
  int offsets[][2]={{239,239},{0,0},{320,480},{-1,-1},{9999,9999},{240,240}};
  for(unsigned frame=0;frame<20;frame++){renderer_ext_media_snapshot(1);
   for(unsigned b=0;b<2;b++)for(unsigned i=0;i<6;i++){pet_surface_node_t n=node(b,offsets[i][0],offsets[i][1]);complete(&n);}
  }assert(attempts==before); /* warm pan + multiple nodes + two sources: zero IO */
 }else if(mode==2){
  load_both();store_generation=2;renderer_ext_media_snapshot(2);
  pet_surface_node_t n=node(1,239,239);memset(framebuffer,0,sizeof(framebuffer));renderer_ext_draw_big(&n);
  /* Old generation pixels cannot leak through even while replacement is partial. */
  for(unsigned i=0;i<640*480;i++)assert(!framebuffer[i]||(framebuffer[i]&31)==8);
  load_both();unsigned before=attempts;complete(&n);assert(attempts==before);
  assert(!renderer_big_slot(2,0,2));assert(!renderer_big_slot(0,16,2));assert(!renderer_big_slot(0,0,0));assert(attempts==before);
 }else if(mode==3||mode==4){
  fail_read=mode==3;fail_decode=mode==4;
  for(unsigned frame=0;frame<3;frame++){renderer_ext_media_snapshot(1);unsigned before=attempts;
   pet_surface_node_t a=node(0,239,239),b=node(1,239,239);renderer_ext_draw_big(&a);renderer_ext_draw_big(&b);
   assert(attempts==before+1);for(unsigned i=0;i<32;i++)assert(!g_big_slots[i].valid);
  }
  fail_read=fail_decode=0;load_both();pet_surface_node_t n=node(1,239,239);complete(&n);
 }else if(mode==5){
  /* Original RGBA must be blended before RGB565 quantization, exactly like
   * an ordinary image. Exercise every alpha and different destination bits. */
  partial=true;pet_surface_node_t n={.big=0,.w=240,.h=240};
  for(sample_alpha=0;sample_alpha<256;sample_alpha++){
   g_big_slots[0].valid=false;renderer_ext_media_snapshot(1);
   uint16_t bg=(uint16_t)(sample_alpha*31337U);
   for(unsigned i=0;i<640*480;i++)framebuffer[i]=bg;
   renderer_ext_draw_big(&n);
   uint16_t want=sample_alpha?blend_rgb565(bg,9,101,37,(uint8_t)sample_alpha):bg;
   for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++)
    assert(framebuffer[y*640+x]==(x<240&&y<240?want:bg));
  }
  /* A concrete regression: r=9, alpha=227 over black is red 1 in RGB565;
   * quantize-expand-before-blend incorrectly made it black. */
  sample_alpha=227;g_big_slots[0].valid=false;renderer_ext_media_snapshot(1);
  memset(framebuffer,0,sizeof(framebuffer));renderer_ext_draw_big(&n);
  assert((framebuffer[0]&0xf800)==0x0800);
 }else assert(0);
 return 0;
}
'''
    source = tmp/'big.c'
    source.write_text(harness)
    binary = tmp/'big'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined','-g','-I',str(ROOT/'main'),
                    '-I',str(ROOT/'tests/vendor/cjson'),str(source),'-o',str(binary)], check=True)
    return binary


@pytest.mark.parametrize('mode', range(6), ids=['twelve-tile-progress','two-source-warm-pan','generation','read-failure','decode-failure','rgba-blending'])
def test_production_big_renderer(big_renderer, mode):
    subprocess.run([str(big_renderer), str(mode)], check=True)


def test_snapshot_budget_handoff_once_before_app_nodes():
    from p4_render_trace_test import function
    source = (ROOT/'main/pet_p4_renderer.c').read_text()
    handoff = function(source, 'renderer_component_media_frame')
    app = function(source, 'render_miniapp_page')
    render = function(source, 'pet_p4_renderer_render')
    assert source.count('renderer_ext_media_snapshot(') == 1
    assert handoff.count('renderer_ext_media_snapshot(') == 1
    assert app.count('renderer_component_media_frame(') == 1
    assert app.index('renderer_component_media_frame(') < app.index('render_package_app(')
    assert render.count('render_miniapp_page(') == 1
    assert 'if(strcmp(page,"app"))component_media_idle=renderer_component_media_frame(NULL);' in render
    fragment = (ROOT/'main/pet_p4_renderer_ext.inc').read_text()
    draw = function(fragment, 'renderer_ext_draw_big')
    assert 'renderer_ext_media_snapshot(' not in draw
    assert 'g_big_read_attempted = false' not in draw
