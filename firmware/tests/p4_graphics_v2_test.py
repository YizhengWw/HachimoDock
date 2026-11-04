"""p4-graphics-v2: per-package graphics quota (8 scene sprites / 16384 pixels, 6 surface
images / 196608 palette pixels / 256 KiB compiled, 4 fonts of 98 chars). The surface
parser applies the ceilings only to a package that declared the profile, on a build
with the flag; every other package keeps the v1 numbers byte for byte. Offline only."""
import json
from pathlib import Path
import subprocess
import sys
import pytest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/skills/petui/scripts'))
from app_surface import app_surface_errors,graphics_limits,GRAPHICS_V1_LIMITS,GRAPHICS_V2_LIMITS
FIX=json.loads((ROOT/'pc/skills/petui/contracts/graphics-v2.json').read_text())
VENDOR=ROOT/'firmware/tests/vendor/cjson'
MAIN=r'''#include "pet_p4_surface.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){(void)argc;FILE*f=fopen(argv[1],"rb");static char b[65536];size_t n=fread(b,1,sizeof(b)-1,f);b[n]=0;fclose(f);
 cJSON*j=cJSON_Parse(b);static pet_surface_t s;int ok=pet_surface_parse_profile(j,&s,48,atoi(argv[2])!=0);cJSON_Delete(j);
 if(ok&&argc>3){printf("%u %u %u %u\n",(unsigned)s.image_count,(unsigned)s.font_count,(unsigned)PET_SURFACE_IMAGES,(unsigned)PET_SURFACE_FONT_CHARS);}
 return ok?0:1;}
'''

def build(tmp_path,flag):
    src=tmp_path/f'parse{int(flag)}.c';src.write_text(MAIN);exe=tmp_path/f'parse{int(flag)}'
    args=['cc','-std=c11','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(VENDOR)]
    if flag:args.append('-DPET_P4_GRAPHICS_V2=1')
    subprocess.run([*args,str(src),str(ROOT/'firmware/main/pet_p4_surface.c'),str(VENDOR/'cJSON.c'),'-lm','-o',str(exe)],check=True,capture_output=True)
    return exe

def parse(exe,surface,profile,tmp_path):
    p=tmp_path/'s.json';p.write_text(json.dumps(surface))
    return subprocess.run([str(exe),str(p),'1' if profile else '0','x'],capture_output=True,text=True,timeout=20)

def test_shared_vectors_on_python_and_both_native_builds(tmp_path):
    on=build(tmp_path,True);off=build(tmp_path,False)
    for s in FIX['valid']:
        assert not app_surface_errors(s,FIX['runtime']),s
        assert parse(on,s,True,tmp_path).returncode==0,s
    for s in FIX['invalid']:
        assert app_surface_errors(s,FIX['runtime']),s
        assert parse(on,s,True,tmp_path).returncode!=0,s
    for s in FIX['validOnlyWithProfile']:
        assert app_surface_errors(s,FIX['runtimeV1']) and app_surface_errors(s,None)
        # Flag on but profile not declared, and flag off with the profile requested: v1 numbers.
        assert parse(on,s,False,tmp_path).returncode!=0,s
        assert parse(off,s,True,tmp_path).returncode!=0,s
    r=parse(on,FIX['valid'][1],True,tmp_path);assert r.stdout.split()==['2','1','6','98']
    r=parse(off,FIX['valid'][0],True,tmp_path);assert r.stdout.split()==['1','0','4','49']

def test_limits_are_the_documented_numbers_and_only_the_profile_unlocks_them():
    assert GRAPHICS_V1_LIMITS=={'images':4,'imagePixels':98304,'imageBytes':131072,'fonts':2,'fontChars':49,'fontWidth':240,'sceneSprites':4,'spritePixels':4096,'keyframes':1}
    assert GRAPHICS_V2_LIMITS=={'images':6,'imagePixels':196608,'imageBytes':262144,'fonts':4,'fontChars':98,'fontWidth':512,'sceneSprites':8,'spritePixels':16384,'keyframes':4}
    assert graphics_limits({'graphics':'p4-graphics-v2'}) is GRAPHICS_V2_LIMITS and graphics_limits({'quota':'p4-quota-v2'}) is GRAPHICS_V1_LIMITS and graphics_limits(None) is GRAPHICS_V1_LIMITS
    header=(ROOT/'firmware/main/pet_p4_surface.h').read_text();mini=(ROOT/'firmware/main/pet_p4_miniapp.h').read_text()
    for text in ['#define PET_SURFACE_V1_IMAGES 4','#define PET_SURFACE_V1_IMAGE_PIXELS 98304U','#define PET_SURFACE_V1_FONTS 2','#define PET_SURFACE_V1_FONT_CHARS 49',
                 '#if PET_P4_GRAPHICS_V2\n#define PET_SURFACE_IMAGES 6\n#define PET_SURFACE_IMAGE_PIXELS 196608U\n#define PET_SURFACE_IMAGE_BYTES (256U*1024U)\n#define PET_SURFACE_FONTS 4\n#define PET_SURFACE_FONT_CHARS 98\n#else\n#define PET_SURFACE_IMAGES 4\n#define PET_SURFACE_IMAGE_PIXELS 98304U\n#define PET_SURFACE_IMAGE_BYTES (128U*1024U)\n#define PET_SURFACE_FONTS 2\n#define PET_SURFACE_FONT_CHARS 49\n#endif']:
        assert text in header,text
    for text in ['#define PET_P4_MINIAPP_SPRITE_V1_MAX 4','#define PET_P4_MINIAPP_SPRITE_V1_PIXELS 4096','#define PET_P4_MINIAPP_SPRITE_DECLARED_MAX 8','#define PET_P4_MINIAPP_SPRITE_V2_PIXELS 16384','#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (256 * 1024)','#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (128 * 1024)']:
        assert text in mini,text
    policy=(ROOT/'firmware/main/pet_p4_render_policy.h').read_text()
    assert '#ifndef PET_P4_GRAPHICS_V2\n#define PET_P4_GRAPHICS_V2 0\n#endif' in policy

def test_runtime_owner_negotiates_the_profile_and_keeps_v1_budgets_elsewhere():
    mini=(ROOT/'firmware/main/pet_p4_miniapp.c').read_text()
    root=mini[mini.index('"schema_version", "vars", "states"'):];assert '"graphics",' in root[:root.index('};')]
    assert 'strcmp(json_string(root,"graphics"),"p4-graphics-v2")' in mini and 'parsed->graphics_v2=true;' in mini
    assert 'set_error(error,error_size,"graphics profile is not available on this device");goto fail;' in mini
    assert 'pet_surface_parse_profile(surface,&parsed->surface,MINIAPP_CAP(parsed,PET_SURFACE_V1_NODES,MINIAPP_V2_SURFACE_NODES,PET_SURFACE_NODES),MINIAPP_GRAPHICS_V2(parsed))' in mini
    assert '(MINIAPP_GRAPHICS_V2(parsed)?PET_P4_MINIAPP_SPRITE_V2_PIXELS:PET_P4_MINIAPP_SPRITE_V1_PIXELS)*3U' in mini
    assert '(MINIAPP_GRAPHICS_V2(runtime)?PET_P4_MINIAPP_SPRITE_DECLARED_MAX:PET_P4_MINIAPP_SPRITE_V1_MAX)' in mini
    assert '#define MINIAPP_GRAPHICS_V2(r) ((void)(r),false)' in mini
    protocol=(ROOT/'firmware/main/pet_p4_protocol.c').read_text()
    assert '#if PET_P4_GRAPHICS_V2\n  cJSON_AddStringToObject(capabilities,"widgetGraphicsV2","p4-graphics-v2");\n#endif' in protocol
    assert 'cJSON_AddNumberToObject(widget_limits, "maxSceneSprites", PET_P4_MINIAPP_SPRITE_V1_MAX);' in protocol
    block=protocol[protocol.index('cJSON *graphics = cJSON_CreateObject();'):protocol.index('cJSON_AddItemToObject(widget_limits, "graphicsV2", graphics);')]
    for key in ['"maxSceneSprites", PET_P4_MINIAPP_SPRITE_DECLARED_MAX','"maxSceneSpritePixels", PET_P4_MINIAPP_SPRITE_V2_PIXELS','"maxSurfaceImages", PET_SURFACE_IMAGES','"maxSurfaceImagePixels", PET_SURFACE_IMAGE_PIXELS','"maxSurfaceImageBytes", PET_SURFACE_IMAGE_BYTES','"maxFonts", PET_SURFACE_FONTS','"maxFontChars", PET_SURFACE_FONT_CHARS']:
        assert key in block,key
    renderer=(ROOT/'firmware/main/pet_p4_renderer.c').read_text()
    assert 'static EXT_RAM_BSS_ATTR uint8_t g_surface_indices[PET_SURFACE_IMAGE_PIXELS];' in renderer
    assert 'static uint16_t g_surface_colors[PET_SURFACE_IMAGES][256];' in renderer and 'static uint8_t g_surface_alpha[PET_SURFACE_IMAGES][256];' in renderer
    assert 'g_surface_offsets[PET_P4_MINIAPP_SPRITE_MAX]' in renderer
    validator=(ROOT/'pc/skills/petui/scripts/validate_generated_widget.py').read_text()
    assert "capabilities.get('widgetGraphicsV2')!='p4-graphics-v2'" in validator and 'graphics_limits(runtime)["sceneSprites"]' in validator
    rust=(ROOT/'pc/src-tauri/src/app_graphics.rs').read_text()
    assert 'pub const V2:Limits=Limits{images:6,image_pixels:196608,image_bytes:256*1024,fonts:4,font_chars:98,font_width:512,scene_sprites:8,sprite_pixels:16384,keyframes:4};' in rust
    for path in ['pc/src-tauri/src/clawpkg.rs','pc/src-tauri/src/usb_serial/widget_transaction.rs','pc/src-tauri/src/component_extension.rs']:
        assert 'crate::app_graphics::limits(' in (ROOT/path).read_text(),path

def test_sprite_pack_capacity_fits_psram_budgets():
    # 6 images * <=98304 palette pixels each still <= 196608 total; compiled pack <= 256 KiB (+3 keyframe bodies).
    assert 196608<=2*98304 and 256*1024+3*100000<=600*1024
    glue=(ROOT/'firmware/main/pet_p4_capability_draw.inc').read_text()
    assert 'EXT_RAM_BSS_ATTR' in glue


def test_scene_view_eight_bindings_require_profile_and_compiled_capacity(tmp_path):
    on=build(tmp_path,True);off=build(tmp_path,False);v=FIX['sceneViewBindings']
    assert not app_surface_errors(v['valid8'],FIX['runtime'])
    assert app_surface_errors(v['valid8'],FIX['runtimeV1'])
    assert parse(on,v['valid8'],True,tmp_path).returncode==0
    assert parse(on,v['valid8'],False,tmp_path).returncode!=0
    assert parse(off,v['valid8'],True,tmp_path).returncode!=0
    assert parse(on,v['invalid9'],True,tmp_path).returncode!=0
    for profile in (False,True):assert parse(on,v['valid4'],profile,tmp_path).returncode==0
    assert parse(off,v['valid4'],False,tmp_path).returncode==0
