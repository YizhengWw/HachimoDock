#include "pet_p4_font_bitmap.h"
#include "pet_p4_font_native32.h"
#if PET_FONT_BITMAP_PACKED
#include "esp_attr.h"
#include "miniz.h"
#include <stddef.h>
#include <string.h>
extern const unsigned char font_packed[] asm("_binary_pet_cn_16_deflate_start");
extern const unsigned char font_packed_end[] asm("_binary_pet_cn_16_deflate_end");
EXT_RAM_BSS_ATTR uint8_t pet_font_bitmap[PET_FONT_BITMAP_BYTES];
static EXT_RAM_BSS_ATTR tinfl_decompressor inflater;
static bool attempted, ready;
#if PET_INSTRUMENT_TONE_PACKED
#include "pet_p4_instrument_tone_table.h"
#include "pet_p4_instrument_tone_packed.h"
/* Same symbol/indexing as the default flash table. Prepared once before audio
 * starts, using the existing boot inflater; no work in the sample loop. */
EXT_RAM_BSS_ATTR int16_t pet_instrument_tone_pcm[PET_INSTRUMENT_TONE_SAMPLES];
_Static_assert(sizeof(pet_instrument_tone_pcm)==PET_INSTRUMENT_TONE_RAW_BYTES,"exact tone output bound");
static bool prepare_instrument_tone(void){
  uint8_t *bytes=(uint8_t*)pet_instrument_tone_pcm;
  size_t consumed=sizeof(pet_instrument_tone_packed),decoded=sizeof(pet_instrument_tone_pcm);
  tinfl_init(&inflater);
  tinfl_status status=tinfl_decompress(&inflater,pet_instrument_tone_packed,&consumed,bytes,bytes,&decoded,
    TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if(status!=TINFL_STATUS_DONE||consumed!=sizeof(pet_instrument_tone_packed)
      ||decoded!=sizeof(pet_instrument_tone_pcm)||tinfl_get_adler32(&inflater)!=PET_INSTRUMENT_TONE_ADLER)return false;
  uint32_t hash=UINT32_C(2166136261);
  for(size_t i=0;i<sizeof(pet_instrument_tone_pcm);i++)hash=(hash^bytes[i])*UINT32_C(16777619);
  if(hash!=PET_INSTRUMENT_TONE_FNV)return false;
  /* The packed representation is canonical LE, not host-endian PCM. Read both
   * bytes before replacing an in-place cell; P4 remains byte-identical. */
  for(size_t i=0;i<PET_INSTRUMENT_TONE_SAMPLES;i++){
    uint16_t bits=(uint16_t)bytes[2*i]|((uint16_t)bytes[2*i+1]<<8);
    memcpy(&pet_instrument_tone_pcm[i],&bits,sizeof(bits));
  }
  return true;
}
#endif
#if PET_FONT_METRICS_PACKED
#if !PET_FONT_NATIVE_32
extern const unsigned char metrics_packed[] asm("_binary_pet_cn_metrics_deflate_start");
extern const unsigned char metrics_packed_end[] asm("_binary_pet_cn_metrics_deflate_end");
#endif
EXT_RAM_BSS_ATTR lv_font_fmt_txt_glyph_dsc_t pet_font_metrics[PET_FONT_METRIC_RECORDS];
#include "pet_p4_font_kerning_sparse.h"
EXT_RAM_BSS_ATTR uint8_t pet_font_kern_left[PET_FONT_METRIC_RECORDS];
EXT_RAM_BSS_ATTR uint8_t pet_font_kern_right[PET_FONT_METRIC_RECORDS];
_Static_assert(PET_FONT_METRIC_RECORDS==PET_FONT_KERN_RECORDS,"complete kerning index space");
static bool prepare_kerning(void){
  memset(pet_font_kern_left,0,sizeof(pet_font_kern_left));
  memset(pet_font_kern_right,0,sizeof(pet_font_kern_right));
  unsigned previous=0;
  for(size_t i=0;i<sizeof(pet_font_kern_pairs)/sizeof(pet_font_kern_pairs[0]);i++){
    const pet_font_kern_pair_t *p=&pet_font_kern_pairs[i];
    if(p->index>=PET_FONT_KERN_RECORDS||(i&&p->index<=previous)||p->left>55||p->right>46)return false;
    pet_font_kern_left[p->index]=p->left;pet_font_kern_right[p->index]=p->right;previous=p->index;
  }
  uint32_t hash=UINT32_C(2166136261);
  for(size_t i=0;i<PET_FONT_KERN_RECORDS;i++)hash=(hash^pet_font_kern_left[i])*UINT32_C(16777619);
  for(size_t i=0;i<PET_FONT_KERN_RECORDS;i++)hash=(hash^pet_font_kern_right[i])*UINT32_C(16777619);
  return hash==PET_FONT_KERN_FNV;
}

_Static_assert(sizeof(pet_font_metrics)==PET_FONT_METRIC_BYTES,"exact metric output bound");
#if !PET_FONT_NATIVE_32
static bool prepare_metrics(void){
  size_t consumed=(size_t)(metrics_packed_end-metrics_packed),decoded=sizeof(pet_font_metrics);
  if(consumed!=PET_FONT_METRIC_PACKED_BYTES)return false;
  uint8_t *bytes=(uint8_t*)pet_font_metrics;
  tinfl_init(&inflater);
  tinfl_status status=tinfl_decompress(&inflater,metrics_packed,&consumed,bytes,bytes,&decoded,
    TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if(status!=TINFL_STATUS_DONE||consumed!=PET_FONT_METRIC_PACKED_BYTES
      ||decoded!=sizeof(pet_font_metrics)||tinfl_get_adler32(&inflater)!=PET_FONT_METRIC_ADLER)return false;
  /* Decode canonical fields explicitly rather than deserialize C bitfields.
   * Read the whole source record before replacing this same fixed-size cell. */
  for(size_t i=0;i<PET_FONT_METRIC_RECORDS;i++){
    const uint8_t *p=bytes+i*8;
    uint32_t word=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    uint32_t index=word&UINT32_C(0xfffff);uint8_t width=p[4],height=p[5];int8_t ox,oy;
    memcpy(&ox,p+6,1);memcpy(&oy,p+7,1);
    uint32_t length=((uint32_t)width*height+7)/8;
    if(index>=PET_FONT_BITMAP_BYTES||length>PET_FONT_BITMAP_BYTES-index)return false;
    pet_font_metrics[i]=(lv_font_fmt_txt_glyph_dsc_t){.bitmap_index=index,.adv_w=word>>20,
      .box_w=width,.box_h=height,.ofs_x=ox,.ofs_y=oy};
  }
  return true;
}
#endif /* legacy metrics */
#if PET_FONT_NATIVE_32
#include "prepare_metrics_columns.inc.h"
#endif
#endif
/* app_main calls before audio/tasks/render. Failure is latched: never retry
 * a decode over a live glyph buffer. No font or decoder pointer leaves C. */
#if PET_FONT_NATIVE_32
static bool prepare_bitmap_after_columns(void){
  size_t consumed=(size_t)(font_packed_end-font_packed),decoded=sizeof(pet_font_bitmap);
  if(consumed!=PET_FONT_BITMAP_PACKED_BYTES)return false;
  tinfl_init(&inflater);
  tinfl_status status=tinfl_decompress(&inflater,font_packed,&consumed,pet_font_bitmap,
    pet_font_bitmap,&decoded,TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  bool bitmap_ok=status==TINFL_STATUS_DONE&&consumed==PET_FONT_BITMAP_PACKED_BYTES
    &&decoded==sizeof(pet_font_bitmap)&&tinfl_get_adler32(&inflater)==PET_FONT_BITMAP_ADLER;
  return bitmap_ok;
}
#endif
bool pet_p4_font_bitmap_init(void){
  if(attempted)return ready;
  attempted=true;
#if PET_FONT_NATIVE_32
  /* Columns borrow the existing bitmap output only before bitmap decode. */
  ready=prepare_metrics_columns()&&prepare_kerning()&&prepare_bitmap_after_columns()
    &&pet_p4_font_native32_prepare(&inflater);
#else
  size_t consumed=(size_t)(font_packed_end-font_packed),decoded=sizeof(pet_font_bitmap);
  if(consumed!=PET_FONT_BITMAP_PACKED_BYTES)return false;
  tinfl_init(&inflater);
  tinfl_status status=tinfl_decompress(&inflater,font_packed,&consumed,pet_font_bitmap,
    pet_font_bitmap,&decoded,TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  bool bitmap_ok=status==TINFL_STATUS_DONE&&consumed==PET_FONT_BITMAP_PACKED_BYTES
    &&decoded==sizeof(pet_font_bitmap)&&tinfl_get_adler32(&inflater)==PET_FONT_BITMAP_ADLER;
#if PET_FONT_METRICS_PACKED
  ready=bitmap_ok&&prepare_metrics()&&prepare_kerning();
#else
  ready=bitmap_ok;
#endif
#endif /* native32/default-off ordering */
#if PET_INSTRUMENT_TONE_PACKED
  ready=ready&&prepare_instrument_tone();
#endif
  if(!ready){
#if PET_FONT_NATIVE_32
    pet_p4_font_native32_clear();
#endif
#if PET_INSTRUMENT_TONE_PACKED
    memset(pet_instrument_tone_pcm,0,sizeof(pet_instrument_tone_pcm));
#endif
    memset(pet_font_bitmap,0,sizeof(pet_font_bitmap));
#if PET_FONT_METRICS_PACKED
    memset(pet_font_metrics,0,sizeof(pet_font_metrics));
    memset(pet_font_kern_left,0,sizeof(pet_font_kern_left));
    memset(pet_font_kern_right,0,sizeof(pet_font_kern_right));
#endif
  }
  return ready;
}
#else
bool pet_p4_font_bitmap_init(void){return true;}
#endif
