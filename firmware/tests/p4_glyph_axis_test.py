"""Existing axis placement only; no change to any raster arithmetic."""
from pathlib import Path
import configparser
import subprocess
import p4_raster_exact_test as raster
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def _run(tmp_path,code,*,sparse=False,wide=False,contract='off',sanitized=True):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    extra='\nstatic float g_glyph_rows[2][66];\nstatic int32_t g_glyph_sample_y;\n'
    extra+=function(text,'app_prepare_glyph_rows')
    if contract=='fast':
        # The release oracle is the actual accepted row-cache parent, not the
        # pre-raster legacy algorithm (which itself differs under fast FMA).
        # Compile independent helpers; changing a later macro cannot change a
        # helper body that was already preprocessed for the other draw path.
        extra+='\n#undef PET_P4_GLYPH_AXIS_INTERNAL\n#define PET_P4_GLYPH_AXIS_INTERNAL 0\n'
        extra+=function(text,'app_glyph_row_alpha').replace('app_glyph_row_alpha(','reference_row_alpha(')
        extra+='\n#undef PET_P4_GLYPH_AXIS_INTERNAL\n#define PET_P4_GLYPH_AXIS_INTERNAL 1\n'
        glyph=function(text,'draw_app_glyph')
        old=glyph.replace('draw_app_glyph(','reference_glyph(')
        assert code.count(old)==1
        code=code.replace(old,old.replace('app_glyph_row_alpha(','reference_row_alpha('))
    extra+=function(text,'app_glyph_row_alpha')
    marker='\n#undef PET_P4_RASTER_EXACT\n#define PET_P4_RASTER_EXACT 0\n'
    assert code.count(marker)==1
    reference_mode=marker.replace('RASTER_EXACT 0','RASTER_EXACT 1') if contract=='fast' else marker
    code=code.replace(marker,extra+reference_mode)
    if sparse:
        assert code.count('return (x*53+y*29)%256;')==1
        code=code.replace('return (x*53+y*29)%256;','return (x+y)%7<5?0:(x*53+y*29)%256;')
        assert code.count(':(x*43+y*71)%256;')==1
        code=code.replace(':(x*43+y*71)%256;',':((x+y)%7<5?0:(x*43+y*71)%256);')
    if wide:
        code=code.replace('.box_w=7+cp%17','.box_w=7+cp%97').replace('f={32,24}','f={32,96}')
    src=tmp_path/'empty.c';exe=tmp_path/'empty';src.write_text(code)
    subprocess.run(['cc','-O2',f'-ffp-contract={contract}','-std=c11','-Wall','-Wextra','-Werror',
      *(['-fsanitize=address,undefined'] if sanitized else []),
      '-DPET_P4_RASTER_EXACT=1','-DPET_P4_ALPHA_FAST=1','-DPET_P4_GLYPH_ROWS=1','-DPET_P4_GLYPH_AXIS_INTERNAL=1',
      '-I',str(ROOT/'main'),str(src),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_actual_glyph_pixels_clipping_weights_release_and_wide_fallback(tmp_path,monkeypatch):
    for contract,sanitized in (('off',True),('fast',False)):
        for sparse,wide in ((False,False),(True,False),(False,True)):
            monkeypatch.setattr(raster,'run',lambda path,code:_run(path,code,sparse=sparse,wide=wide,contract=contract,sanitized=sanitized))
            raster.test_actual_glyph_all_sampling_paths_are_bit_exact(tmp_path)


def test_actual_nonempty_expression_is_bit_exact_with_and_without_fma(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    alpha=function(text,'app_glyph_row_alpha')
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static float g_glyph_rows[2][66];
#define PET_P4_GLYPH_AXIS_INTERNAL 0
'''+alpha.replace('app_glyph_row_alpha(','reference_alpha(')
    code+='\n#undef PET_P4_GLYPH_AXIS_INTERNAL\n#define PET_P4_GLYPH_AXIS_INTERNAL 1\n'+alpha
    code+=r'''
int main(void){
 const float samples[]={0,1,17,127,254,255};
 const float fractions[]={0,0x1p-149f,.125f,.3333333f,.5f,.875f,0x1.fffffep-1f,1};
 unsigned checked=0;
 for(unsigned a=0;a<6;a++)for(unsigned b=0;b<6;b++)for(unsigned c=0;c<6;c++)for(unsigned d=0;d<6;d++){
  g_glyph_rows[0][1]=samples[a];g_glyph_rows[0][2]=samples[b];
  g_glyph_rows[1][1]=samples[c];g_glyph_rows[1][2]=samples[d];
  for(unsigned x=0;x<8;x++)for(unsigned y=0;y<8;y++){
   float expected=reference_alpha(0,fractions[x],fractions[y]);
   float actual=app_glyph_row_alpha(0,fractions[x],fractions[y]);
   assert(memcmp(&expected,&actual,sizeof(actual))==0);checked++;
  }
 }
 assert(checked==82944);return 0;
}
'''
    # Memory instrumentation inserts checks between source loads, changing the
    # legal contraction choices under Clang's fast mode. It is NOT the release
    # arithmetic oracle. Keep every bit-equality assertion: separately exercise
    # instrumented strict arithmetic AND uninstrumented release contraction.
    for contract,sanitized in (('off',True),('off',False),('fast',False),('fast',True)):
        src=tmp_path/f'rounding-{contract}-{sanitized}.c';exe=tmp_path/f'rounding-{contract}-{sanitized}';src.write_text(code)
        subprocess.run(['cc','-O2',f'-ffp-contract={contract}','-std=c11','-Wall','-Wextra','-Werror',
          *(['-fsanitize=address,undefined'] if sanitized else []),str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=60)


def test_private_axis_placement_keeps_gates_and_has_no_new_scratch(tmp_path):
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_glyph_axis_parent':
            assert config[section]['extends']=='env:esp32_p4_retained_parent'
            assert 'PET_P4_GLYPH_AXIS_INTERNAL=1' in flags
            assert not any(v in flags for v in ('TASK_PRIORITY','PROGRAM_EXECUTION','F_CPU','PERIPHERAL','BUDGET_US'))
        else:assert 'PET_P4_GLYPH_AXIS_INTERNAL=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'static DRAM_ATTR pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];' in text
    assert 'g_glyph_columns' not in text
    assert '#define PET_P4_RENDER_FRAME_BUDGET_US 66667U' in text
    assert 'logical_unchanged=pet_p4_logical_frame_unchanged(' in text
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_rgb565_blend.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_GLYPH_AXIS_INTERNAL=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Glyph axis placement requires the exact-raster parent' in result.stderr
