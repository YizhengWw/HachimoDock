"""Bounded clean-frame warming; partial bytes must never authorize a frame."""
from pathlib import Path
import subprocess
import p4_app_dirty_cache_test as dirty
ROOT=Path(__file__).resolve().parents[1]


def test_actual_renderer_progressively_warms_slow_frames_without_pixel_changes(tmp_path,monkeypatch):
    original=dirty.run
    def progressive(path,code):
        code='#define PET_P4_APP_CACHE_PROGRESSIVE 1\n#define PET_P4_APP_CACHE_POPULATION_BUDGET 1\n'+code
        marker='strcpy(app.metric_value,"123");compare(&app,0);'
        assert code.count(marker)==1
        code=code.replace(marker,marker+r'''
 assert(!g_surface_background_valid&&!g_app_frame_key.valid);
 for(unsigned n=1;n<8;n++)compare(&app,n);
 assert(g_surface_background_valid&&g_app_frame_key.valid);
 compare(&app,8);assert(labels==0); /* Exact warm frame performs no labels. */
 /* The clock budget no longer causes indefinite warming deferral. No clock
  * helper is supplied: the progressive branch is fixed work, not elapsed time. */
 for(unsigned n=8;n<32;n++){
   if(n%3==0)snprintf(app.metric_value,sizeof(app.metric_value),"%u",n);
   compare(&app,n);
 }
 assert(g_surface_background_valid&&g_app_frame_key.valid);
''')
        # The inherited adversarial phase changes page/background/viewport on
        # nearly every frame. It proves pixel identity, not fewer glyph draws
        # during an intentionally interrupted warm-up. The ORIGINAL dirty-only
        # test still enforces its original aggregate label-saving assertion.
        marker=' assert(partial_labels<full_labels);'
        assert code.count(marker)==1
        code=code.replace(marker,' /* Pixel identity checked on every adversarial frame above. */')
        original(path,code)
    monkeypatch.setattr(dirty,'run',progressive)
    dirty.test_actual_dirty_renderer_matches_full_pixels_with_order_clips_forms_and_overlays(tmp_path)


def test_progressive_rows_invalidation_revision_and_quantum(tmp_path):
    code=r'''
#define PET_P4_APP_DIRTY_CACHE 1
#define PET_P4_APP_CACHE_PROGRESSIVE 1
#include "pet_p4_app_cache_progressive.h"
#include <assert.h>
static uint16_t source[640*480],cache[640*480];
static pet_app_frame_key_t key;
int main(void){
 _Static_assert(sizeof(pet_app_cache_progressive_t)<=72,"bounded renderer state");
 pet_surface_t s={.app=true,.graphics=true,.count=1};
 s.nodes[0]=(pet_surface_node_t){.type=1,.page=4,.bind=4,.x=40,.y=100,.w=200,.h=100,.icon=-1};
 const char *v[9]={"","","","","0","","","",""};
 pet_app_cache_progressive_t p={0};
 for(unsigned i=0;i<640*480;i++){source[i]=(uint16_t)(i*31337);cache[i]=0;}
 unsigned total=0;
 for(unsigned n=0;n<8;n++){
   pet_app_cache_progressive_prepare(&p,&key,&s,v,0,0,7,false);
   assert(!key.valid);
   unsigned copied=pet_app_cache_progressive_copy(&p,&key,cache,source);
   assert(copied<=64);total+=copied;
   assert(key.valid==(n==7));
 }
 assert(total==480&&!memcmp(cache,source,sizeof(cache)));
 /* A 100-row field changes while warming; unaffected rows remain exact. */
 v[4]="1";for(unsigned y=100;y<200;y++)for(unsigned x=40;x<240;x++)source[y*640+x]^=0xffff;
 pet_app_cache_progressive_prepare(&p,&key,&s,v,0,0,7,true);
 assert(p.count==380&&!key.valid);
 assert(pet_app_cache_progressive_copy(&p,&key,cache,source)==64&&!key.valid);
 v[4]="2";for(unsigned y=100;y<200;y++)for(unsigned x=40;x<240;x++)source[y*640+x]^=0x1234;
 pet_app_cache_progressive_prepare(&p,&key,&s,v,0,0,7,false);
 assert(p.count==380);
 for(unsigned n=0;n<2;n++){pet_app_cache_progressive_prepare(&p,&key,&s,v,0,0,7,false);assert(pet_app_cache_progressive_copy(&p,&key,cache,source)<=64);}
 assert(key.valid&&!memcmp(cache,source,sizeof(cache)));
 /* Empty image packs/resource changes still invalidate every cached row. */
 pet_app_cache_progressive_prepare(&p,&key,&s,v,0,0,8,false);
 assert(p.count==0&&!key.valid);
 pet_app_cache_progressive_copy(&p,&key,cache,source);
 pet_app_cache_progressive_prepare(&p,&key,&s,v,1,0,8,false);
 assert(p.count==0&&!key.valid);
 pet_app_cache_progressive_copy(&p,&key,cache,source);
 s.image_count=1;
 pet_app_cache_progressive_prepare(&p,&key,&s,v,1,0,8,false);
 assert(p.count==0&&!key.valid);
 pet_app_cache_progressive_cancel(&p);assert(!p.active&&p.count==0);
 assert(pet_app_cache_progressive_copy(&p,&key,cache,source)==0);
 return 0;
}
'''
    src=tmp_path/'rows.c';binary=tmp_path/'rows';src.write_text(code)
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),
      str(src),str(ROOT/'main/pet_p4_surface.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),
      '-Wno-deprecated-declarations','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)
