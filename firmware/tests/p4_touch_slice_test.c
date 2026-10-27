#include <assert.h>
#include <stdio.h>
#include "pet_p4_touch_core.h"
#include "pet_p4_slice_core.h"

static void decoder_tests(void) {
  /* CST826 shares this report layout: DOWN (0), contact (2), UP (1).
   * A stale second slot must not become a second finger when count is one. */
  uint8_t cst[13] = {1, 0, 120, 0x11, 44, 0, 0, 0xff, 0xff, 0xff, 0xff};
  pet_p4_ft6336_frame_t cst_frame;
  assert(pet_p4_ft6336_decode(cst, sizeof(cst), &cst_frame));
  assert(cst_frame.count == 1 && cst_frame.x == 120 && cst_frame.y == 300 && cst_frame.track_id == 1);
  cst[1] = 0x80;
  assert(pet_p4_ft6336_decode(cst, sizeof(cst), &cst_frame) && cst_frame.count == 1);
  cst[1] = 0x40;
  assert(pet_p4_ft6336_decode(cst, sizeof(cst), &cst_frame) && cst_frame.count == 0);
  uint8_t raw[13]={1,0x81,0xdf,0x32,0x7f};
  pet_p4_ft6336_frame_t frame;
  assert(pet_p4_ft6336_decode(raw,sizeof(raw),&frame));
  assert(frame.count==1 && frame.x==479 && frame.y==639 && frame.track_id==3);
  raw[1]=0x41; assert(pet_p4_ft6336_decode(raw,sizeof(raw),&frame) && !frame.count);
  raw[1]=0xc1; assert(!pet_p4_ft6336_decode(raw,sizeof(raw),&frame));
  raw[1]=0x81;raw[2]=0xe0;assert(!pet_p4_ft6336_decode(raw,sizeof(raw),&frame));
  raw[0]=3;assert(!pet_p4_ft6336_decode(raw,sizeof(raw),&frame));
  assert(!pet_p4_ft6336_decode(raw,1,&frame));
  raw[0]=0;assert(pet_p4_ft6336_decode(raw,sizeof(raw),&frame) && !frame.count);
  raw[0]=2;raw[2]=0xdf;raw[7]=0x80;raw[8]=30;raw[9]=0x40;raw[10]=50;
  assert(pet_p4_ft6336_decode(raw,sizeof(raw),&frame) && frame.count==2);
  pet_p4_touch_decoder_t d; pet_p4_touch_event_t g;
  pet_p4_touch_decoder_init(&d, 48, 700);
  assert(!pet_p4_touch_decoder_update(&d, true, 10, 10, 1, &g));
  assert(!pet_p4_touch_decoder_update(&d, true, 80, 80, 800, &g));
  assert(pet_p4_touch_decoder_update(&d, false, 80, 80, 815, &g));
  assert(g.gesture == PET_P4_TOUCH_SWIPE_RIGHT);
  pet_p4_touch_decoder_init(&d, 48, 700);
  pet_p4_touch_decoder_update(&d, true, 50, 50, 1, &g);
  pet_p4_touch_decoder_update(&d, true, 75, 50, 40, &g);
  pet_p4_touch_decoder_update(&d, true, 50, 50, 60, &g);
  assert(!pet_p4_touch_decoder_update(&d, false, 50, 50, 70, &g));
  pet_p4_touch_decoder_init(&d, 48, 700);
  pet_p4_touch_decoder_update(&d, true, 50, 50, 1, &g);
  assert(pet_p4_touch_decoder_update(&d, true, 50, 50, 701, &g));
  assert(g.gesture == PET_P4_TOUCH_LONG_PRESS);
  assert(!pet_p4_touch_decoder_update(&d, false, 50, 50, 702, &g));
  int x,y; pet_p4_touch_panel_to_ui(0,0,&x,&y); assert(x==0 && y==479);
  pet_p4_touch_panel_to_ui(479,639,&x,&y); assert(x==639 && y==0);
  pet_p4_touch_cst826_to_ui(0,639,&x,&y); assert(x==0 && y==0);
  pet_p4_touch_cst826_to_ui(479,639,&x,&y); assert(x==0 && y==479);
  pet_p4_touch_cst826_to_ui(0,0,&x,&y); assert(x==639 && y==0);
  pet_p4_touch_cst826_to_ui(479,0,&x,&y); assert(x==639 && y==479);
  /* Left-edge top-to-bottom stroke stays on the left and moves downward. */
  pet_p4_touch_cst826_to_ui(40,600,&x,&y); assert(x==39 && y==40);
  pet_p4_touch_cst826_to_ui(400,600,&x,&y); assert(x==39 && y==400);
  pet_p4_pointer_decoder_t p={0}; pet_p4_pointer_event_t e;
  assert(pet_p4_pointer_update(&p,true,true,100,200,10,&e) && e.kind==PET_P4_POINTER_DOWN);
  assert(pet_p4_pointer_update(&p,true,true,100,200,25,&e) && e.kind==PET_P4_POINTER_MOVE);
  assert(pet_p4_pointer_update(&p,false,false,100,200,40,&e) && e.kind==PET_P4_POINTER_CANCEL);
  assert(!pet_p4_pointer_update(&p,true,false,100,200,55,&e));
  assert(pet_p4_pointer_update(&p,true,true,100,200,70,&e) && e.kind==PET_P4_POINTER_DOWN);
  assert(pet_p4_pointer_update(&p,true,true,500,200,240,&e) && e.kind==PET_P4_POINTER_CANCEL);
  assert(pet_p4_pointer_update(&p,true,true,500,200,255,&e) && e.kind==PET_P4_POINTER_DOWN);
  assert(pet_p4_pointer_update(&p,true,false,0,0,270,&e) && e.kind==PET_P4_POINTER_UP && e.x==500);
}
static bool point(pet_p4_slice_t *s, pet_p4_pointer_kind_t k, int x, int y, uint64_t t) {
  pet_p4_pointer_event_t e={k,x,y,t}; return pet_p4_slice_pointer(s,&e);
}
int main(void) {
  decoder_tests();
  pet_p4_slice_t s;
  pet_p4_slice_start(&s,0);
  assert(!point(&s,PET_P4_POINTER_DOWN,260,460,1));
  assert(!point(&s,PET_P4_POINTER_MOVE,260,460,16)); /* stationary is not a cut */
  point(&s,PET_P4_POINTER_CANCEL,260,460,17);
  point(&s,PET_P4_POINTER_DOWN,180,460,20);
  assert(point(&s,PET_P4_POINTER_MOVE,360,460,35)); /* endpoints outside fruit */
  assert(s.score==1 && s.sliced);
  assert(!point(&s,PET_P4_POINTER_MOVE,180,460,50) && s.score==1);
  pet_p4_slice_start(&s,0);
  point(&s,PET_P4_POINTER_DOWN,180,460,1);
  assert(!point(&s,PET_P4_POINTER_MOVE,360,460,200)); /* stale */
  point(&s,PET_P4_POINTER_UP,360,460,201);
  assert(!point(&s,PET_P4_POINTER_MOVE,180,460,210)); /* new contact */
  assert(!point(&s,PET_P4_POINTER_MOVE,700,460,220)); /* malformed bounds */
  assert(!s.pointer_down && s.score==0);
  pet_p4_slice_start(&s,0);
  for (unsigned t=15;t<10000;t+=15) pet_p4_slice_tick(&s,t);
  assert(s.finished && !s.running && s.misses==3);
  pet_p4_slice_start(&s,10000);
  assert(s.running && !s.finished && s.score==0 && s.misses==0 && s.trail_count==0);
  for (unsigned t=10015;t<11500;t+=15) {
    point(&s,t==10015?PET_P4_POINTER_DOWN:PET_P4_POINTER_MOVE,20,20,t);
    pet_p4_slice_tick(&s,t);
    assert(s.pointer_down && s.trail_count<=PET_P4_SLICE_TRAIL_MAX);
  }
  pet_p4_slice_tick(&s,12000); assert(!s.pointer_down);
  puts("touch and slice behavior passed");
}
