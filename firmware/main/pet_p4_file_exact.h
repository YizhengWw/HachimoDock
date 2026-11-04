/* Native bounded filesystem-byte comparison, not checksum/existence authority.
 * No writes, allocation, retry or retained FILE pointer. Caller owns the product
 * HAL cache bypass and storage lifecycle; SDK filesystem cache coherency stays
 * unchanged. This is not raw-media scrubbing. current() is trusted native code,
 * never package supplied. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef PET_P4_FILE_EXACT_TRACE_ENABLE
#define PET_P4_FILE_EXACT_TRACE_ENABLE 0
#endif
#if PET_P4_FILE_EXACT_TRACE_ENABLE
#include "pet_p4_widget_trace.h"
#endif
enum {PET_FILE_EXACT_CANCELLED=-1,PET_FILE_EXACT_DIFFERENT=0,PET_FILE_EXACT_MATCH=1};
static inline int pet_file_exact(const char *path,const void *first,size_t first_size,
    const void *second,size_t second_size,bool (*current)(void)){
  if(!path||!current||(!first&&first_size)||(!second&&second_size))return PET_FILE_EXACT_DIFFERENT;
  if(!current())return PET_FILE_EXACT_CANCELLED;
#if PET_P4_FILE_EXACT_TRACE_ENABLE
  PET_WIDGET_TRACE_SCOPE(open_scope,PET_WTRACE_FS_OPEN);
#endif
  FILE *file=fopen(path,"rb");
#if PET_P4_FILE_EXACT_TRACE_ENABLE
  pet_widget_trace_scope_end(&open_scope);
#endif
  if(!file)return current()?PET_FILE_EXACT_DIFFERENT:PET_FILE_EXACT_CANCELLED;
  uint8_t bytes[256];int result=PET_FILE_EXACT_MATCH;
  const uint8_t *parts[]={first,second};size_t sizes[]={first_size,second_size};
  for(unsigned part=0;part<2&&result==PET_FILE_EXACT_MATCH;part++){
    size_t offset=0;
    while(offset<sizes[part]){
      if(!current()){result=PET_FILE_EXACT_CANCELLED;break;}
      size_t n=sizes[part]-offset;if(n>sizeof(bytes))n=sizeof(bytes);
#if PET_P4_FILE_EXACT_TRACE_ENABLE
      PET_WIDGET_TRACE_SCOPE(read_scope,PET_WTRACE_FS_DATA);
#endif
      size_t got=fread(bytes,1,n,file);
#if PET_P4_FILE_EXACT_TRACE_ENABLE
      pet_widget_trace_scope_end(&read_scope);
#endif
      if(got!=n||ferror(file)||memcmp(bytes,parts[part]+offset,n)){
        result=PET_FILE_EXACT_DIFFERENT;break;
      }
      offset+=n;
    }
  }
#if PET_P4_FILE_EXACT_TRACE_ENABLE
  PET_WIDGET_TRACE_SCOPE(eof_scope,PET_WTRACE_FS_DATA);
#endif
  if(result==PET_FILE_EXACT_MATCH&&(fgetc(file)!=EOF||ferror(file)))result=PET_FILE_EXACT_DIFFERENT;
#if PET_P4_FILE_EXACT_TRACE_ENABLE
  pet_widget_trace_scope_end(&eof_scope);
  PET_WIDGET_TRACE_SCOPE(close_scope,PET_WTRACE_FS_CLOSE);
#endif
  if(fclose(file)!=0&&result==PET_FILE_EXACT_MATCH)result=PET_FILE_EXACT_DIFFERENT;
#if PET_P4_FILE_EXACT_TRACE_ENABLE
  pet_widget_trace_scope_end(&close_scope);
#endif
  return current()?result:PET_FILE_EXACT_CANCELLED;
}
