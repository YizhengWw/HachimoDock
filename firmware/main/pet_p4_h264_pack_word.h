/* Private default-off aligned word IO. Native/physical acceptance separate.
 * A complete 8Y/4chroma block and nonoverlapping writable12byte target are
 * native caller-owned. Failed pointer/alignment check performs NO write.
 * Ordinary memcpy preserves effective-type rules; no uint32 pointer punning. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifndef PET_P4_H264_PACK_WORD_IO
#define PET_P4_H264_PACK_WORD_IO 0
#endif
static inline __attribute__((always_inline)) bool pet_h264_pack8_word_io(
  uint8_t *target,const uint8_t *y,const uint8_t *chroma){
  if(!target||!y||!chroma||(((uintptr_t)target|(uintptr_t)y|(uintptr_t)chroma)&3U))return false;
#if defined(__GNUC__) || defined(__clang__)
  uint8_t *at=__builtin_assume_aligned(target,4);
  const uint8_t *ay=__builtin_assume_aligned(y,4),*ac=__builtin_assume_aligned(chroma,4);
#else
  uint8_t *at=target;const uint8_t *ay=y,*ac=chroma;
#endif
  uint32_t y_lo,y_hi,uv;
  memcpy(&y_lo,ay,4);memcpy(&y_hi,ay+4,4);memcpy(&uv,ac,4);
  uint32_t first=(uv&UINT32_C(0xff))|((y_lo&UINT32_C(0xffff))<<8)|((uv&UINT32_C(0xff00))<<16);
  uint32_t second=(y_lo>>16)|(uv&UINT32_C(0xff0000))|((y_hi&UINT32_C(0xff))<<24);
  uint32_t third=((y_hi>>8)&UINT32_C(0xff))|((uv>>16)&UINT32_C(0xff00))|(y_hi&UINT32_C(0xffff0000));
  memcpy(at,&first,4);memcpy(at+4,&second,4);memcpy(at+8,&third,4);
  return true;
}
