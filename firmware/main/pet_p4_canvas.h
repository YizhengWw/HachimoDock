/* Trusted bounded drawing interpreter, not package code or framebuffer access.
 * Immutable commands/values remain owned by one lifecycle. Callbacks are trusted
 * native rendering code, never supplied by packages, and cannot mutate inputs.
 * Validate completes before the first draw; outputs are clipped to the canvas.
 * No allocation, IO, floating point, clocks, DMA, or retries. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_CANVAS_V2
#define PET_P4_CANVAS_V2 0
#endif
#ifndef PET_CANVAS_ENABLED
#if PET_P4_CANVAS_V2
#define PET_CANVAS_ENABLED 1
#else
#define PET_CANVAS_ENABLED 0
#endif
#endif
#if PET_P4_CANVAS_V2 && !PET_CANVAS_ENABLED
#error Canvas v2 requires the bounded canvas interpreter
#endif
/* p4-canvas-v1 budgets; they also bound every v1 declaration on a v2 build. */
#define PET_CANVAS_V1_COMMANDS 16U
#define PET_CANVAS_V1_VARS 8U
#define PET_CANVAS_V1_SPANS 2048U
#if PET_P4_CANVAS_V2
/* p4-canvas-v2 array capacities and software budget (cost table in canvas.md). */
#define PET_CANVAS_COMMANDS 64U
#define PET_CANVAS_VARS 16U
#define PET_CANVAS_SPANS 8192U
#define PET_CANVAS_POINTS 64U
#define PET_CANVAS_POLYLINE_MIN 3U
#define PET_CANVAS_POLYLINE_MAX 16U
#else
#define PET_CANVAS_COMMANDS 16U
#define PET_CANVAS_VARS 8U
#define PET_CANVAS_SPANS 2048U
#endif
enum {PET_CANVAS_LINE,PET_CANVAS_RECT,PET_CANVAS_CIRCLE
#if PET_P4_CANVAS_V2
,PET_CANVAS_POLYLINE,PET_CANVAS_ARC,PET_CANVAS_RING
#endif
};
/* Nonnegative operands are constants. -1..-N are native binding indices, not
 * pointers. A package names declared int variables; the trusted parser resolves
 * these indices. Negative runtime values clamp to zero, never wrap.
 * v2: polyline a=first pooled point, b=point count; arc a=radius, c/d=start/end
 * degrees; ring a=outer radius, b=inner radius. */
typedef struct {int16_t x,y,a,b;uint16_t color;uint8_t op;
#if PET_P4_CANVAS_V2
int16_t c,d;
#endif
} pet_canvas_command_t;
typedef struct {
  uint16_t x,y,w,h;
  uint8_t count,variable_count,page;
#if PET_P4_CANVAS_V2
  uint8_t v2,point_count;
  int16_t points[PET_CANVAS_POINTS][2];
#endif
  pet_canvas_command_t commands[PET_CANVAS_COMMANDS];
  char names[PET_CANVAS_VARS][32];
  int32_t values[PET_CANVAS_VARS];
} pet_canvas_t;
#if PET_P4_CANVAS_V2
#define PET_CANVAS_IS_V2(c) ((c)->v2!=0)
#else
#define PET_CANVAS_IS_V2(c) ((void)(c),false)
#endif
typedef void (*pet_canvas_rect_fn)(void *context,int x,int y,int w,int h,uint16_t color);
bool pet_canvas_valid(const pet_canvas_t *canvas);
/* Budgets of one declaration: v1 numbers for p4-canvas-v1, v2 numbers for p4-canvas-v2. */
unsigned pet_canvas_command_limit(const pet_canvas_t *canvas);
unsigned pet_canvas_variable_limit(const pet_canvas_t *canvas);
unsigned pet_canvas_span_limit(const pet_canvas_t *canvas);
/* Zero on invalid arguments/declaration. Valid empty/fully clipped draw can
 * also emit zero spans; validation must be checked separately if needed. */
unsigned pet_canvas_draw(const pet_canvas_t *canvas,pet_canvas_rect_fn rect,void *context);
#if PET_P4_CANVAS_V2
/* Screen-space degrees 0..359 of (dx,dy), y down, clockwise; integer only. The
 * same function bounds arcs on the device and in the preview core. */
unsigned pet_canvas_angle(int dx,int dy);
#endif
