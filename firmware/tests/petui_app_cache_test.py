"""Exercise the real exact-match cache key; no hash collision/stale UI shortcut."""
import subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def test_cache_invalidation_for_every_visible_input(tmp_path):
    source=tmp_path/'cache.c';binary=tmp_path/'cache'
    source.write_text(r'''
#include "pet_p4_app_cache.h"
#include <assert.h>
static pet_app_frame_key_t key;
int main(void){
 pet_surface_t s={0};s.app=true;s.viewport.enabled=true;s.count=1;s.nodes[0].x=20;s.nodes[0].w=100;s.nodes[0].h=60;
 const char*values[9]={"Title","Date","Main","Label","2/7","Unit","Badge","Note","Footer"};
 assert(!pet_app_frame_matches(&key,&s,values,0,20));pet_app_frame_remember(&key,&s,values,0,20);
 assert(pet_app_frame_matches(&key,&s,values,0,20));
 for(int i=0;i<9;i++){const char*old=values[i];values[i]="Changed";assert(!pet_app_frame_matches(&key,&s,values,0,20));values[i]=old;}
 assert(!pet_app_frame_matches(&key,&s,values,1,20));assert(!pet_app_frame_matches(&key,&s,values,0,21));
 pet_surface_t changed=s;
#define CHECK(field,value) changed=s;changed.field=value;assert(!pet_app_frame_matches(&key,&changed,values,0,20));
 CHECK(viewport.offset,120);CHECK(nodes[0].hidden,true);CHECK(nodes[0].disabled,true);CHECK(nodes[0].pressed,true);CHECK(nodes[0].color,65535);CHECK(nodes[0].bound_value,42);CHECK(nodes[0].tile_index,2);
 changed=s;strcpy(changed.nodes[0].text,"New record");assert(!pet_app_frame_matches(&key,&changed,values,0,20));
 CHECK(nodes[0].rotate_ms,1000);CHECK(nodes[0].floating,true);CHECK(game_view,true);CHECK(scene_view.enabled,true);CHECK(data_view.enabled,true);CHECK(viewport.enabled,false);CHECK(app,false);
 assert(pet_app_frame_matches(&key,&s,values,0,20));key.valid=false;assert(!pet_app_frame_matches(&key,&s,values,0,20));
 s.viewport.enabled=false;s.viewport_count=3;for(int i=0;i<3;i++)s.viewports[i].enabled=true;
 assert(pet_app_frame_eligible(&s));pet_app_frame_remember(&key,&s,values,0,20);assert(pet_app_frame_matches(&key,&s,values,0,20));
 for(int i=0;i<3;i++){changed=s;changed.viewports[i].offset=120;assert(!pet_app_frame_matches(&key,&changed,values,0,20));}
 assert(pet_app_frame_matches(&key,&s,values,0,20));
 s.viewport_count=0;assert(pet_app_frame_eligible(&s));
 strcpy(s.nodes[0].field_bind,"mode");strcpy(s.nodes[0].text,"Quiet");
 pet_app_frame_remember(&key,&s,values,0,20);assert(pet_app_frame_matches(&key,&s,values,0,20));
 changed=s;strcpy(changed.nodes[0].text,"Normal");assert(!pet_app_frame_matches(&key,&changed,values,0,20));
 return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_panel_optimization_preserves_rounded_edges_opacity_gradient_and_stroke(tmp_path):
    import re
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    def function(name):
        start=re.search(r'^static [^\n;{]*\b'+name+r'\([^;{]*\)\s*\{',text,re.M).start();begin=text.index('{',start);depth=1;end=begin+1
        while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]+'\n'
    code='''#include "pet_p4_surface.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t pixels[640*480],expected[640*480],*g_framebuffer=pixels;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
'''+function('blend_rgb565')
    code+=text[text.index('static struct {bool active;int x,y,right,bottom;} g_app_clip;'):text.index('static void put_px(')]
    code+=''.join(function(n)for n in ['put_px','fill_rect','blend_px','draw_app_panel'])
    code+=r'''
static void reference(const pet_surface_node_t*n){
 const uint8_t r=((n->color>>11)&31)*255/31,g=((n->color>>5)&63)*255/63,b=(n->color&31)*255/31;
 for(int y=0;y<n->h;y++)for(int x=0;x<n->w;x++){
  float qx=fabsf(x+.5f-n->w*.5f)-(n->w*.5f-n->radius),qy=fabsf(y+.5f-n->h*.5f)-(n->h*.5f-n->radius);
  float dx=fmaxf(qx,0),dy=fmaxf(qy,0),d=sqrtf(dx*dx+dy*dy)+fminf(fmaxf(qx,qy),0)-n->radius;
  float cover=fminf(1,fmaxf(0,.5f-d));if(cover<=0)continue;
  int pos=n->vertical?y:x,len=n->vertical?n->h:n->w,a=n->opacity+(n->opacity_end-n->opacity)*pos/(len>1?len-1:1);
  if(a)blend_px(n->x+x,n->y+y,r,g,b,(int)(a*cover));
  if(n->stroke_width){float edge=fminf(1,fmaxf(0,d+n->stroke_width+.5f))*cover;uint16_t c=n->stroke;if(edge>0)blend_px(n->x+x,n->y+y,((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31,(int)(edge*255));}
 }
}
int main(void){
 for(int row=-40;row<481;row+=260)for(int radius=0;radius<=24;radius+=12)for(int a=0;a<=255;a+=85)for(int stroke=0;stroke<=2;stroke+=2)for(int vertical=0;vertical<2;vertical++){
  pet_surface_node_t n={.x=8,.y=row,.w=161,.h=81,.radius=radius,.color=0xe38f,.opacity=a,.opacity_end=255,.stroke_width=stroke,.stroke=0x07e0,.vertical=vertical};
  g_app_clip.active=true;g_app_clip.x=16;g_app_clip.y=64;g_app_clip.right=624;g_app_clip.bottom=416;
  for(int p=0;p<640*480;p++)pixels[p]=0x1357;
  reference(&n);memcpy(expected,pixels,sizeof(expected));for(int p=0;p<640*480;p++)pixels[p]=0x1357;
  draw_app_panel(&n);assert(!memcmp(expected,pixels,sizeof(expected)));
 }
 return 0;
}
'''
    source=tmp_path/'panel.c';binary=tmp_path/'panel';source.write_text(code)
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(source),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def test_opaque_panel_spans_match_original_pixels_and_reduce_distance_work(tmp_path):
    """Exercise production spans against the pre-span renderer, not a new rasterizer."""
    import re
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    def function(name):
        start=re.search(r'^static [^\n;{]*\b'+name+r'\([^;{]*\)\s*\{',text,re.M).start();begin=text.index('{',start);depth=1;end=begin+1
        while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
        return text[start:end]+'\n'
    panel=function('draw_app_panel')
    # This original span-budget case exercises the actual default-off build.
    # Preprocess its real compile guard before instrumenting the one active
    # SDF expression; private mirrored pixels have their own differential suite.
    panel=subprocess.run(['cc','-E','-P','-x','c','-DPET_P4_APP_PANEL_MIRROR=0','-'],
        input=panel,text=True,capture_output=True,check=True).stdout
    assert panel.count('float qx=')==1
    panel=panel.replace('float qx=','distance_evaluations++;float qx=')
    code='''#include "pet_p4_surface.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t pixels[640*480],expected[640*480],*g_framebuffer=pixels;
static unsigned distance_evaluations,reference_evaluations;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
'''+function('blend_rgb565')
    code+=text[text.index('static struct {bool active;int x,y,right,bottom;} g_app_clip;'):text.index('static void put_px(')]
    code+=''.join(function(n)for n in ['put_px','fill_rect','blend_px'])+panel
    code+=r'''
/* Frozen production algorithm before opaque spans: retain clipping, SDF,
 * gradient rounding, the opaque store, and stroke compositing exactly. */
static void reference(const pet_surface_node_t *n) {
  if(!n->opacity&&!n->opacity_end&&!n->stroke_width)return;
  if(!n->radius&&!n->stroke_width&&n->opacity==255&&n->opacity_end==255){fill_rect(n->x,n->y,n->w,n->h,n->color);return;}
  const uint8_t r=((n->color>>11)&31)*255/31,g=((n->color>>5)&63)*255/63,b=(n->color&31)*255/31;
  const int radius=n->radius;
  int x0=n->x<0?-n->x:0,y0=n->y<0?-n->y:0,x1=n->w,y1=n->h;
  if(n->x+x1>640)x1=640-n->x;
  if(n->y+y1>480)y1=480-n->y;
  if(g_app_clip.active){if(n->x+x0<g_app_clip.x)x0=g_app_clip.x-n->x;if(n->y+y0<g_app_clip.y)y0=g_app_clip.y-n->y;if(n->x+x1>g_app_clip.right)x1=g_app_clip.right-n->x;if(n->y+y1>g_app_clip.bottom)y1=g_app_clip.bottom-n->y;}
  for(int y=y0;y<y1;y++){
    float qy=fabsf(y+.5f-n->h*.5f)-(n->h*.5f-radius);
    for(int x=x0;x<x1;x++){
      reference_evaluations++;
      float qx=fabsf(x+.5f-n->w*.5f)-(n->w*.5f-radius);
      float d=(qx>0&&qy>0?sqrtf(qx*qx+qy*qy):(qx>qy?qx:qy))-radius;
      float cover=.5f-d;if(cover<=0)continue;if(cover>1)cover=1;
      int pos=n->vertical?y:x,len=n->vertical?n->h:n->w;
      int a=n->opacity+(n->opacity_end-n->opacity)*pos/(len>1?len-1:1);
      if(a==255&&cover==1)put_px(n->x+x,n->y+y,n->color);
      else if(a)blend_px(n->x+x,n->y+y,r,g,b,(int)(a*cover));
      if(n->stroke_width){float edge=fminf(1,fmaxf(0,d+n->stroke_width+.5f))*cover;uint16_t c=n->stroke;if(edge>0)blend_px(n->x+x,n->y+y,((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31,(int)(edge*255));}
    }
  }
}
static uint32_t random_state=0x70dd41a9;
static uint32_t random_u32(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void background(unsigned seed){for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)((p*31337U+seed)^(p>>3));}
static void compare(const pet_surface_node_t *n,unsigned seed){
  background(seed);reference_evaluations=0;reference(n);memcpy(expected,pixels,sizeof(expected));
  background(seed);distance_evaluations=0;draw_app_panel(n);
  if(memcmp(expected,pixels,sizeof(expected))){
    fprintf(stderr,"panel mismatch seed=%u xy=%d,%d wh=%d,%d r=%u opacity=%u/%u stroke=%u clip=%d:%d,%d,%d,%d\n",seed,n->x,n->y,n->w,n->h,n->radius,n->opacity,n->opacity_end,n->stroke_width,g_app_clip.active,g_app_clip.x,g_app_clip.y,g_app_clip.right,g_app_clip.bottom);
    abort();
  }
  if(n->opacity!=255||n->opacity_end!=255||n->stroke_width||n->radius>n->w/2||n->radius>n->h/2)
    assert(distance_evaluations==reference_evaluations);
  else assert(distance_evaluations<=reference_evaluations);
}
int main(void){
  /* Actual acceptance panel dimensions: all AA corners remain evaluated. */
  pet_surface_node_t n={.x=24,.y=100,.w=284,.h=238,.radius=16,.color=6371,.opacity=255,.opacity_end=255};
  compare(&n,1);assert(reference_evaluations==284*238&&distance_evaluations==4*16*16);
  n.x=332;compare(&n,2);assert(reference_evaluations==284*238&&distance_evaluations==4*16*16);
  printf("two acceptance panels: distance evaluations 135184 -> 2048; exact pixels\n");
  /* Include one-pixel/odd/even dimensions, maximal legal radii, empty center
   * strips, viewport clipping, fully/partially off-screen, and arbitrary RGB565. */
  for(unsigned i=0;i<1200;i++){
    n.x=(int)(random_u32()%1100)-300;n.y=(int)(random_u32()%800)-200;
    n.w=1+random_u32()%640;n.h=1+random_u32()%480;
    int cap=n.w<n.h?n.w/2:n.h/2;if(cap>24)cap=24;
    n.radius=random_u32()%(cap+1);n.color=(uint16_t)random_u32();n.stroke=(uint16_t)random_u32();n.vertical=random_u32()&1;
    n.opacity=n.opacity_end=255;n.stroke_width=0;
    if(i%5==1)n.stroke_width=1+random_u32()%2;
    if(i%5==2)n.opacity=n.opacity_end=random_u32()%255;
    if(i%5==3){n.opacity=random_u32()%255;n.opacity_end=random_u32()%256;}
    if(i%5==4){n.w=1+random_u32()%31;n.h=1+random_u32()%31;n.radius=24;} /* malformed radius stays on old path */
    g_app_clip.active=random_u32()&1;g_app_clip.x=(int)(random_u32()%900)-200;g_app_clip.y=(int)(random_u32()%700)-150;
    g_app_clip.right=g_app_clip.x+random_u32()%700;g_app_clip.bottom=g_app_clip.y+random_u32()%500;
    compare(&n,100+i);
  }
  for(int radius=1;radius<=24;radius++)for(int extra=0;extra<2;extra++){
    n=(pet_surface_node_t){.x=17,.y=19,.w=2*radius+extra,.h=2*radius+extra,.radius=radius,.color=(uint16_t)random_u32(),.opacity=255,.opacity_end=255};
    g_app_clip.active=false;compare(&n,2000+radius*2+extra);
    g_app_clip.active=true;g_app_clip.x=17+radius;g_app_clip.y=19+radius;g_app_clip.right=18+radius;g_app_clip.bottom=20+radius;compare(&n,3000+radius*2+extra);
  }
  printf("1298 deterministic differential cases passed (no device timing claim)\n");
  return 0;
}
'''
    source=tmp_path/'panel_spans.c';binary=tmp_path/'panel_spans';source.write_text(code)
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(source),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

def _exercise_production_app_cache(tmp_path,node_trace=False,cells=False,bridge=False):
    """Run the production cache branch, not a second implementation of it.

    Drawing stubs count expensive raster work and mark pixels. Form label and
    draft/commit semantics use the actual C editor; no viewport is declared.
    """
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    render=text[text.index('static bool render_package_app('):text.index('static bool render_package_media(')]
    # Keep the real compile-time drawing adapter (default-off here), rather
    # than inventing a test stub that could hide an active candidate path.
    canvas_adapter=text[text.index('#if PET_CANVAS_ENABLED',text.index('#include "pet_p4_app_cache.h"')):text.index('static EXT_RAM_BSS_ATTR pet_app_frame_key_t g_app_frame_key;')]
    trace_adapter=text[text.index('#if PET_P4_APP_NODE_TRACE\n_Static_assert(PET_SURFACE_NODES'):text.index('static void record_render_performance(')]
    render=render.replace('static bool render_package_app(', 'static bool render_captured_app(')
    code=r'''
#include "pet_p4_app_cache.h"
#include "pet_p4_app_form.h"
#include "pet_p4_cells.h"
#include "pet_p4_render_trace.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#if PET_P4_APP_NODE_TRACE
static pet_p4_render_trace_t g_render_trace;
static int64_t trace_clock;
static uint32_t trace_frame;
static char trace_log[1024];
static int64_t esp_timer_get_time(void){return ++trace_clock;}
static void captured_trace_log(const char *format,...){va_list ap;va_start(ap,format);vsnprintf(trace_log,sizeof(trace_log),format,ap);va_end(ap);}
#define TAG 0
#define ESP_LOGI(tag,...) captured_trace_log(__VA_ARGS__)
#define ESP_LOGD(tag,...) captured_trace_log(__VA_ARGS__)
#define ESP_LOGW(tag,...) captured_trace_log(__VA_ARGS__)
#endif
#define EXT_RAM_BSS_ATTR
#define PET_P4_GAME_NONE 0
typedef struct {int kind;} game_t;
typedef struct {
 char widget_id[48],title[64],eyebrow[96],headline[160],metric_label[96],metric_value[64],metric_unit[32],badge[16],note[160],footer[160],data_source[48];
 uint8_t surface_page;int progress_percent;bool slicing,media,instrument;game_t game;struct{bool enabled;}data;
} pet_p4_miniapp_view_t;
static struct {pet_surface_t surface;pet_cells_t cells;bool surface_active,side_active,confirmation_active;char confirmation_prompt[121];}g_miniapp_render_snapshot;
static uint16_t pixels[640*480],*g_framebuffer=pixels,g_surface_background[640*480];
static bool g_surface_background_valid,reload_images,side_active,confirmation;
static uint8_t g_surface_background_page;
static const pet_surface_t *g_app_text_surface;
static struct {bool active;int x,y,right,bottom;}g_app_clip;
static pet_app_frame_key_t g_app_frame_key;
static pet_surface_t fixture;
static pet_form_t form;
static int32_t vars[PET_FORM_VARS]={1,1,4,4,0,0};
static unsigned labels,panels,confirm_checks,side_frames;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static bool fixture_surface(pet_surface_t*s){
 *s=fixture;
 for(int i=0;i<s->count;i++){
  pet_surface_node_t*n=&s->nodes[i];
  if(n->field_bind[0])pet_form_text(&form,vars,pet_form_field_index(&form,n->field_bind),n->text);
  if(n->var_bind[0]){int v=n->var_bind[0]-'0';n->bound_value=vars[v];snprintf(n->text,sizeof(n->text),"%ld",(long)vars[v]);}
  int op=pet_form_operation(&form,n->action);if(op>=0)n->disabled=!pet_form_allowed(&form,vars,0,op);
 }
 return true;
}
static void load_surface_images(unsigned long long now){if(reload_images){g_surface_background_valid=false;reload_images=false;}}
static void fill_rect(int x,int y,int w,int h,uint16_t color){for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)pixels[j*640+i]=color;}
static void draw_app_game(const pet_surface_t*s,const game_t*g){}
static void draw_app_scene(const pet_surface_t*s,const game_t*g,unsigned long long now){}
static void draw_app_data(const pet_surface_t*s,const pet_p4_miniapp_view_t*a){}
static void draw_app_side(void){if(side_active)pixels[17]=(uint16_t)++side_frames;}
#if PET_P4_CELLS_V1
static unsigned cell_frames;
static void draw_app_cells(const pet_surface_node_t*n,const pet_cells_t*c){cell_frames++;pixels[n->y*640+n->x]=c->values[0];}
#endif
#if PET_SERVICE_BRIDGE_ENABLED
static unsigned image_pixel=101,image_calls;static bool image_available=true;
static bool draw_service_image(const pet_surface_node_t*n,const char*id,unsigned long long now){
 if(!n->image_bind[0])return false;image_calls++;if(!image_available)return false;
 pixels[n->y*640+n->x]=(uint16_t)image_pixel;return true;
}
#endif
static void draw_surface_image(const pet_surface_node_t*n){panels++;pixels[n->y*640+n->x]=77;}
static void draw_app_panel(const pet_surface_node_t*n){panels++;pixels[n->y*640+n->x]=n->color;}
static void draw_app_icon(int icon,int x,int y,int size,uint16_t color){pixels[y*640+x]=color;}
static void draw_app_label(const pet_surface_node_t*n,const char*t,uint16_t color){
 labels++;unsigned hash=color;for(;*t;t++)hash=hash*33+(unsigned char)*t;pixels[n->y*640+n->x]=(uint16_t)hash;
}
static const char*draw_text_line(const char*t,int x,int y,int w,uint16_t c,int scale,bool last){return t+strlen(t);}
static void blend_round_rect(int x,int y,int w,int h,int radius,unsigned r,unsigned g,unsigned b,unsigned a){pixels[y*640+x]^=(uint16_t)a;}
static void blend_rect(int x,int y,int w,int h,unsigned r,unsigned g,unsigned b,unsigned a){pixels[640*480-1]=0xaaaa;}
static void fill_round_rect(int x,int y,int w,int h,int radius,uint16_t color){pixels[y*640+x]=color;}
static bool fixture_confirmation(char*out,unsigned long long now){confirm_checks++;if(confirmation)strcpy(out,"Confirm now");return confirmation;}
'''+trace_adapter+canvas_adapter+render+r'''
/* Fixtures supply an already-captured frame; the production renderer must not
 * invoke any miniapp getter after this boundary. */
static bool render_package_app(unsigned long long now,const pet_p4_miniapp_view_t*app){
#if PET_P4_APP_NODE_TRACE
 pet_p4_render_trace_begin(&g_render_trace,++trace_frame,"app");g_render_trace.app_draw_started_us=1;
#endif
 g_miniapp_render_snapshot.surface_active=fixture_surface(&g_miniapp_render_snapshot.surface);
 g_miniapp_render_snapshot.side_active=side_active;
 g_miniapp_render_snapshot.confirmation_active=fixture_confirmation(g_miniapp_render_snapshot.confirmation_prompt,now);
 bool rendered=render_captured_app(now,app);
#if PET_P4_APP_NODE_TRACE
 assert(!g_render_trace.app_nodes.invalid_clock);
 if(g_render_trace.app_nodes.cache_hit)assert(!g_render_trace.app_nodes.nodes_mask);
 app_node_trace_log(&g_render_trace);assert(strstr(trace_log,"P4 app nodes frame="));
#endif
 return rendered;
}
static void fresh(void){g_app_frame_key.valid=false;g_surface_background_valid=false;}
static void stable(pet_p4_miniapp_view_t*app){unsigned before=labels;assert(render_package_app(100,app));assert(labels>before);before=labels;assert(render_package_app(200,app));assert(labels==before);}
static void changed(pet_p4_miniapp_view_t*app){stable(app);}
int main(void){
 form.enabled=true;form.count=2;form.page=0;form.focus_var=4;form.active_var=5;
 strcpy(form.fields[0].id,"mode");form.fields[0].var=0;form.fields[0].draft_var=1;form.fields[0].option_count=2;form.fields[0].values[0]=1;form.fields[0].values[1]=2;strcpy(form.fields[0].labels[0],"Quiet");strcpy(form.fields[0].labels[1],"Normal");
 strcpy(form.fields[1].id,"level");form.fields[1].var=2;form.fields[1].draft_var=3;form.fields[1].min=0;form.fields[1].max=10;form.fields[1].step=2;
 const char*actions[]={"previous","next","decrease","increase","accept","cancel"};for(int i=0;i<6;i++)strcpy(form.actions[i],actions[i]);
 fixture.app=fixture.graphics=fixture.variable_bindings=true;fixture.background=123;fixture.count=10;
 for(int i=0;i<10;i++){pet_surface_node_t*n=&fixture.nodes[i];n->type=1;n->page=4;n->x=20;n->y=20+i*36;n->w=240;n->h=32;n->bind=255;n->icon=-1;n->font_size=20;}
 strcpy(fixture.nodes[0].field_bind,"mode");strcpy(fixture.nodes[1].field_bind,"level");
 strcpy(fixture.nodes[2].var_bind,"4");strcpy(fixture.nodes[3].var_bind,"5");
 for(int i=4;i<10;i++)strcpy(fixture.nodes[i].action,actions[i-4]);
 pet_p4_miniapp_view_t app={0};fresh();stable(&app);
 unsigned before=labels;for(int i=0;i<120;i++)assert(render_package_app(300+i,&app));assert(labels==before);
 assert(pet_form_apply(&form,vars,0,PET_FORM_ACCEPT));changed(&app); /* edit + enable */
 uint16_t old_choice=pixels[20*640+20];assert(pet_form_apply(&form,vars,0,PET_FORM_INCREASE));changed(&app);assert(pixels[20*640+20]!=old_choice); /* field_bind alone */
 assert(pet_form_apply(&form,vars,0,PET_FORM_CANCEL));changed(&app);assert(pixels[20*640+20]==old_choice);
 assert(pet_form_apply(&form,vars,0,PET_FORM_NEXT));changed(&app); /* focus */
 assert(pet_form_apply(&form,vars,0,PET_FORM_ACCEPT));changed(&app);
 assert(pet_form_apply(&form,vars,0,PET_FORM_INCREASE));changed(&app); /* integer draft */
 assert(pet_form_apply(&form,vars,0,PET_FORM_ACCEPT));changed(&app);assert(vars[2]==6&&vars[5]==0);
 fixture.nodes[0].pressed=true;changed(&app);fixture.nodes[0].pressed=false;changed(&app);
 fixture.nodes[0].hidden=true;changed(&app);fixture.nodes[0].hidden=false;changed(&app);
 strcpy(app.headline,"new timer value");changed(&app);app.progress_percent=20;changed(&app);
 app.surface_page=1;changed(&app);app.surface_page=0;changed(&app);
 reload_images=true;changed(&app); /* image revision invalidates shared buffer */
 before=labels;unsigned overlays=confirm_checks;confirmation=true;assert(render_package_app(600,&app));assert(labels==before+3&&pixels[640*480-1]==0xaaaa&&confirm_checks==overlays+1);
 confirmation=false;before=labels;assert(render_package_app(700,&app));assert(labels==before&&pixels[640*480-1]==123); /* overlay not baked */
 pixels[640*480-1]=0xbbbb;assert(render_package_app(800,&app));assert(pixels[640*480-1]==123); /* outer connection/session overlay not baked */
 /* Runtime layers outside surface must never hit an identical cached key. */
 for(int kind=0;kind<7;kind++){
  fresh();stable(&app);if(kind==0)app.slicing=true;if(kind==1)app.media=true;if(kind==2)app.instrument=true;if(kind==3)app.game.kind=1;if(kind==4)app.data.enabled=true;if(kind==5)strcpy(app.data_source,"live");if(kind==6)side_active=true;
  before=labels;assert(render_package_app(900,&app));assert(labels>before);before=labels;assert(render_package_app(950,&app));assert(labels>before&&!g_app_frame_key.valid);
  app.slicing=app.media=app.instrument=app.data.enabled=false;app.game.kind=0;app.data_source[0]=0;side_active=false;
 }
 assert(side_frames==2);
 /* Intrinsic surface animation remains live, as do scene/data/game surfaces. */
 for(int kind=0;kind<5;kind++){
  fresh();if(kind==0)fixture.nodes[0].floating=true;if(kind==1)fixture.nodes[0].rotate_ms=1000;if(kind==2)fixture.game_view=true;if(kind==3)fixture.scene_view.enabled=true;if(kind==4)fixture.data_view.enabled=true;
  before=labels;assert(render_package_app(1000,&app));assert(render_package_app(1100,&app));assert(labels>=before+2*fixture.count&&!g_app_frame_key.valid);
  fixture.nodes[0].floating=false;fixture.nodes[0].rotate_ms=0;fixture.game_view=fixture.scene_view.enabled=fixture.data_view.enabled=false;
 }
#if PET_P4_CELLS_V1
 /* Unchanged surface/dashboard with only runtime grid mutations must repaint.
  * A static panel prefix may still be cached independently. */
 fixture=(pet_surface_t){.app=true,.graphics=true,.count=2,.background=123};
 fixture.nodes[0]=(pet_surface_node_t){.type=0,.page=4,.x=1,.y=1,.w=20,.h=20,.color=77};
 fixture.nodes[1]=(pet_surface_node_t){.type=PET_SURFACE_NODE_CELLS,.page=4,.x=50,.y=50,.w=32,.h=32};
 g_miniapp_render_snapshot.cells=(pet_cells_t){.enabled=true,.cols=2,.rows=2,.bits=2};
 app=(pet_p4_miniapp_view_t){0};fresh();
 for(unsigned i=1;i<=8;i++){
  g_miniapp_render_snapshot.cells.values[0]=(uint8_t)(i%4);
  assert(render_package_app(2000+i,&app));
  assert(pixels[50*640+50]==i%4&&!g_app_frame_key.valid);
 }
 assert(cell_frames==8);
 /* These two legal 4-bit 4x4 grids collide under the runtime's FNV change hint.
  * Equal resolved bound_value and surface bytes cannot prove equal pixels. */
 static const uint8_t collisions[2][16]={{3,0,7,4,1,9,5,4,9,5,10,15,14,0,12,7},{12,0,4,14,5,8,1,8,8,2,14,3,12,3,7,12}};
 g_miniapp_render_snapshot.cells.cols=g_miniapp_render_snapshot.cells.rows=4;g_miniapp_render_snapshot.cells.bits=4;
 fixture.nodes[1].bound_value=1240888036;
 for(unsigned i=0;i<2;i++){
  uint32_t hash=2166136261u;for(unsigned k=0;k<16;k++)hash=(hash^collisions[i][k])*16777619u;
  assert(hash==1240888036u);
  memcpy(g_miniapp_render_snapshot.cells.values,collisions[i],16);
  assert(render_package_app(3000+i,&app));assert(pixels[50*640+50]==collisions[i][0]);
 }
#endif
#if PET_SERVICE_BRIDGE_ENABLED
 fixture=(pet_surface_t){.app=true,.graphics=true,.count=1,.background=123};
 fixture.nodes[0]=(pet_surface_node_t){.type=6,.page=4,.x=70,.y=70,.w=32,.h=32};
 strcpy(fixture.nodes[0].image_bind,"cover");fresh();
 for(unsigned i=0;i<3;i++){
  /* Production render_snapshot folds the host image epoch into bound_value. */
  image_pixel=101+i;fixture.nodes[0].bound_value=1+i;assert(render_package_app(4000+i,&app));
  assert(pixels[70*640+70]==image_pixel&&g_app_frame_key.valid);
 }
 /* Unavailable/retired lease must clear the old pixels via the live fallback. */
 image_available=false;fixture.nodes[0].bound_value=0;assert(render_package_app(4003,&app));
 assert(image_calls==4&&pixels[70*640+70]==77);
 assert(render_package_app(4004,&app));assert(image_calls==4); /* same epoch may cache */
#endif
#if PET_P4_CELLS_V1 || PET_SERVICE_BRIDGE_ENABLED
 /* An external node that cannot draw must not disable the static current page.
  * Visibility restoration still invalidates that authority before any live draw. */
 fixture=(pet_surface_t){.app=true,.graphics=true,.count=3,.background=123};
 fixture.nodes[0]=(pet_surface_node_t){.type=0,.page=4,.x=1,.y=1,.w=20,.h=20,.color=77};
 fixture.nodes[1]=(pet_surface_node_t){.type=1,.page=4,.x=25,.y=25,.w=80,.h=30,.bind=255,.icon=-1};
 strcpy(fixture.nodes[1].text,"Static page");app=(pet_p4_miniapp_view_t){0};
 pet_surface_node_t *unseen=&fixture.nodes[2];
 *unseen=(pet_surface_node_t){.page=4,.x=50,.y=50,.w=32,.h=32};
#if PET_P4_CELLS_V1
 unseen->type=PET_SURFACE_NODE_CELLS;
#else
 unseen->type=6;strcpy(unseen->image_bind,"cover");image_available=true;
#endif
 for(unsigned mode=0;mode<3;mode++){
  unseen->hidden=mode==0;unseen->page=mode==1?1:4;unseen->in_view=mode==2;unseen->y=mode==2?300:50;
  fixture.viewport=(pet_surface_viewport_t){.enabled=mode==2,.w=320,.h=240,.content_height=720};
  fresh();unsigned l0=labels,p0=panels;
  assert(render_package_app(5000+mode*2,&app)&&render_package_app(5001+mode*2,&app));
  assert(labels-l0==1&&panels-p0==1&&g_app_frame_key.valid);
 }
 unseen->hidden=false;unseen->page=4;unseen->in_view=false;unseen->y=50;
 unsigned l0=labels;assert(render_package_app(5010,&app)&&render_package_app(5011,&app));
#if PET_P4_CELLS_V1
 assert(labels-l0==2&&!g_app_frame_key.valid);
#else
 assert(labels-l0==1&&g_app_frame_key.valid); /* borrowed image uses its epoch key */
#endif
#endif
 return 0;
}
'''
    source=tmp_path/'render-cache.c';binary=tmp_path/'render-cache';source.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined',f'-DPET_P4_APP_NODE_TRACE={int(node_trace)}',f'-DPET_P4_CELLS_V1={int(cells)}',f'-DPET_SERVICE_BRIDGE_ENABLED={int(bridge)}','-DPET_P4_APP_PHASE_TRACE=1','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(source),str(ROOT/'main/pet_p4_surface.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_production_app_renderer_caches_plain_bound_forms_but_keeps_overlays_live(tmp_path):
    _exercise_production_app_cache(tmp_path)


def test_production_app_cache_keeps_identical_form_and_overlay_semantics_with_node_trace(tmp_path):
    _exercise_production_app_cache(tmp_path,node_trace=True)


def test_production_app_cache_does_not_freeze_runtime_cell_updates(tmp_path):
    _exercise_production_app_cache(tmp_path,cells=True)


def test_production_app_cache_tracks_live_service_image_availability(tmp_path):
    _exercise_production_app_cache(tmp_path,bridge=True)
