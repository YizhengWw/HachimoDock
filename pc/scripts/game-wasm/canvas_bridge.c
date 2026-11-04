/* Trusted fixed-memory ABI, no imports or package-provided functions. */
#include "pet_p4_canvas.h"
#include <stddef.h>
void *memset(void *p,int v,size_t n){unsigned char *s=p;while(n--)*s++=(unsigned char)v;return p;}
static pet_canvas_t canvas;
static int32_t output[1+PET_CANVAS_SPANS*5];
void canvas_begin(int x,int y,int w,int h,int count,int variables){
  memset(&canvas,0,sizeof(canvas));canvas.x=x;canvas.y=y;canvas.w=w;canvas.h=h;canvas.count=count;canvas.variable_count=variables;
}
void canvas_command(unsigned index,int op,int x,int y,int a,int b,int color){
  if(index<PET_CANVAS_COMMANDS)canvas.commands[index]=(pet_canvas_command_t){.x=x,.y=y,.a=a,.b=b,.color=color,.op=op};
}
int canvas_valid(void){return pet_canvas_valid(&canvas);}
void canvas_value(unsigned index,int32_t value){if(index<PET_CANVAS_VARS)canvas.values[index]=value;}
/* Span capacity of this core build (2048 for a v1-only core, 8192 with p4-canvas-v2). */
int canvas_spans_max(void){return (int)PET_CANVAS_SPANS;}
#if PET_P4_CANVAS_V2
/* p4-canvas-v2: declaration profile, pooled polyline points and arc angles. */
void canvas_v2(int on){canvas.v2=on?1:0;}
void canvas_point(unsigned index,int x,int y){
  if(index<PET_CANVAS_POINTS){canvas.points[index][0]=x;canvas.points[index][1]=y;if(canvas.point_count<=index)canvas.point_count=index+1;}
}
void canvas_command_extra(unsigned index,int c,int d){
  if(index<PET_CANVAS_COMMANDS){canvas.commands[index].c=c;canvas.commands[index].d=d;}
}
unsigned canvas_angle(int dx,int dy){return pet_canvas_angle(dx,dy);}
#endif
static void emit(void *ctx,int x,int y,int w,int h,uint16_t color){
  (void)ctx;unsigned n=(unsigned)output[0];if(n>=PET_CANVAS_SPANS)return;
  int32_t *p=output+1+5*n;p[0]=x;p[1]=y;p[2]=w;p[3]=h;p[4]=color;output[0]++;
}
int32_t *canvas_render(void){output[0]=0;pet_canvas_draw(&canvas,emit,NULL);return output;}
/* p4-particles-v1: two emitter slots of the same integer core as the device. */
#include "pet_p4_particles.h"
static pet_particles_config_t particle_config[2];
static pet_particles_state_t particle_state[2];
static int32_t particle_output[1+PET_PARTICLES_MAX_DOTS*4];
void particles_begin(unsigned slot,int x,int y,int w,int h,int color,int life_ms,int tick_ms,int spread,int count,int budget,int speed,int shape,int gravity,int seed){
  if(slot>=2)return;
  particle_config[slot]=(pet_particles_config_t){.x=x,.y=y,.w=w,.h=h,.color=color,.life_ms=life_ms,.tick_ms=tick_ms,.spread=spread,.count=count,.budget=budget,.speed=speed,.shape=shape,.gravity=gravity};
  pet_particles_reset(&particle_state[slot],(uint32_t)seed);
}
int particles_valid(unsigned slot){return slot<2&&pet_particles_valid(&particle_config[slot]);}
unsigned particles_burst(unsigned slot){return slot<2?pet_particles_burst(&particle_config[slot],&particle_state[slot]):0;}
unsigned particles_step(unsigned slot,unsigned now_ms,int emitting){return slot<2?pet_particles_step(&particle_config[slot],&particle_state[slot],now_ms,emitting!=0):0;}
static void particle_dot(void *ctx,int x,int y,int size,uint16_t color){
  (void)ctx;unsigned n=(unsigned)particle_output[0];if(n>=PET_PARTICLES_MAX_DOTS)return;
  int32_t *p=particle_output+1+4*n;p[0]=x;p[1]=y;p[2]=size;p[3]=color;particle_output[0]++;
}
int32_t *particles_dots(unsigned slot){particle_output[0]=0;if(slot<2)pet_particles_draw(&particle_config[slot],&particle_state[slot],particle_dot,NULL);return particle_output;}
