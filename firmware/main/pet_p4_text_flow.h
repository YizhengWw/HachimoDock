/* Deterministic long-text layout (p4-longtext-v1) shared by the device, the PC
 * preview and the validators. The metric model is normative, not the glyph
 * width table: every scalar >= U+2E80 (CJK, fullwidth forms) advances one
 * font size, everything else half a font size (rounded up). Lines break after
 * spaces, before/after wide scalars and at '\n'. No allocation, no IO. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PET_TEXT_FLOW_MAX_BYTES 4096U
#define PET_TEXT_FLOW_MAX_LINES 256U
#define PET_TEXT_FLOW_LINE_BYTES 120U
typedef struct {uint16_t start,end;} pet_text_line_t;
static inline unsigned pet_text_flow_scalar(const unsigned char *s,uint32_t *cp){
  unsigned n=s[0]<128?1:s[0]>=0xc2&&s[0]<=0xdf?2:s[0]>=0xe0&&s[0]<=0xef?3:s[0]>=0xf0&&s[0]<=0xf4?4:0;
  if(!n)return 0;uint32_t v=s[0]&(n==1?127:n==2?31:n==3?15:7);
  for(unsigned j=1;j<n;j++){if((s[j]&0xc0)!=0x80)return 0;v=(v<<6)|(s[j]&63);}
  if((n==2&&v<128)||(n==3&&v<2048)||(n==4&&v<65536)||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
  *cp=v;return n;
}
/* Complete UTF-8, no C0/C1 controls except '\n', at most max bytes. */
static inline bool pet_text_flow_valid(const char *text,unsigned max){
  if(!text||max>PET_TEXT_FLOW_MAX_BYTES)return false;
  unsigned i=0;
  while(i<=max){
    if(!text[i])return true;if(i==max)return false;
    uint32_t cp;unsigned n=pet_text_flow_scalar((const unsigned char*)text+i,&cp);
    if(!n||n>max-i||(cp<32&&cp!='\n')||(cp>=127&&cp<160))return false;
    i+=n;
  }
  return false;
}
static inline bool pet_text_flow_wide(uint32_t cp){return cp>=0x2e80;}
static inline unsigned pet_text_flow_advance(uint32_t cp,unsigned font_size){return pet_text_flow_wide(cp)?font_size:(font_size+1)/2;}
/* Greedy wrap into byte ranges. Trailing spaces are trimmed from a line and
 * leading spaces are skipped after a wrap. Returns the line count; *truncated
 * is set when the cap stopped the layout (remaining text is not laid out). */
static inline unsigned pet_text_flow_lines(const char *text,unsigned width,unsigned font_size,pet_text_line_t *out,unsigned max_lines,bool *truncated){
  unsigned count=0,i=0,line_start=0,line_width=0,last_break=0;bool more=false;
  if(truncated)*truncated=false;
  if(!text||!out||!max_lines||width<font_size||font_size<10)return 0;
  #define PET_TEXT_FLOW_EMIT(end_at) do{unsigned e=(end_at);while(e>line_start&&text[e-1]==' ')e--;\
    if(count<max_lines){out[count].start=(uint16_t)line_start;out[count].end=(uint16_t)e;count++;}else more=true;}while(0)
  while(text[i]){
    uint32_t cp;unsigned n=pet_text_flow_scalar((const unsigned char*)text+i,&cp);
    if(!n)break;
    if(cp=='\n'){PET_TEXT_FLOW_EMIT(i);i+=n;line_start=i;line_width=0;last_break=0;if(more)break;continue;}
    unsigned w=pet_text_flow_advance(cp,font_size);
    if(pet_text_flow_wide(cp)&&i>line_start)last_break=i;
    bool restart=false;
    while((line_width+w>width||i+n-line_start>PET_TEXT_FLOW_LINE_BYTES)&&i>line_start){
      unsigned cut=last_break>line_start?last_break:i;
      PET_TEXT_FLOW_EMIT(cut);if(more)break;
      line_start=cut;while(text[line_start]==' ')line_start++;
      line_width=0;last_break=0;
      for(unsigned k=line_start;k<i;){uint32_t c;unsigned m=pet_text_flow_scalar((const unsigned char*)text+k,&c);if(!m)break;line_width+=pet_text_flow_advance(c,font_size);k+=m;}
      if(line_start>i){i=line_start;restart=true;break;}
    }
    if(more)break;
    if(restart)continue;
    line_width+=w;i+=n;
    if(cp==' '||pet_text_flow_wide(cp))last_break=i;
  }
  if(!more&&(i>line_start||count==0)){PET_TEXT_FLOW_EMIT(i);}
  #undef PET_TEXT_FLOW_EMIT
  if(truncated)*truncated=more;
  return count;
}
static inline unsigned pet_text_flow_per_page(unsigned h,unsigned line_height){unsigned n=line_height?h/line_height:0;return n?n:1;}
static inline unsigned pet_text_flow_pages(unsigned lines,unsigned per_page){unsigned p=per_page?(lines+per_page-1)/per_page:1;return p?p:1;}
/* Signed modulo page selection, like the data list pager. */
static inline unsigned pet_text_flow_page_index(int32_t page_value,unsigned pages){if(!pages)return 0;int64_t p=page_value%(int64_t)pages;if(p<0)p+=pages;return (unsigned)p;}
/* Copies one laid-out line (at most PET_TEXT_FLOW_LINE_BYTES) into out. */
static inline void pet_text_flow_copy(const char *text,const pet_text_line_t *line,char *out,unsigned capacity){
  if(!out||!capacity)return;
  unsigned n=line->end>line->start?line->end-line->start:0;if(n>=capacity)n=capacity-1;
  /* Never cut a scalar: back up to a boundary. */
  while(n&&((unsigned char)text[line->start+n]&0xc0)==0x80)n--;
  memcpy(out,text+line->start,n);out[n]=0;
}
