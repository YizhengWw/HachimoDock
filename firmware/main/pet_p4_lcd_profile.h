/* Default-off measurement only. No deadline, buffer or completion policy. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_RENDER_LCD_SPLIT_PROBE
#define PET_P4_RENDER_LCD_SPLIT_PROBE 0
#endif
typedef struct {uint32_t submit_us,wait_us;bool valid,submitted,refresh_waited;} pet_lcd_observation_t;
static inline uint32_t pet_lcd_elapsed(int64_t begin,int64_t end,bool *valid){
  if(begin<0||end<begin||(uint64_t)end-(uint64_t)begin>UINT32_MAX){*valid=false;return 0;}
  return (uint32_t)((uint64_t)end-(uint64_t)begin);
}
