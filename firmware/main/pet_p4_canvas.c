#include "pet_p4_canvas.h"
static bool operand(int value,unsigned limit,unsigned variables,bool size){
  return value<0?(unsigned)(-value)<=variables:(unsigned)value<=limit&&(!size||value>0);
}
unsigned pet_canvas_command_limit(const pet_canvas_t *c){return c&&PET_CANVAS_IS_V2(c)?PET_CANVAS_COMMANDS:PET_CANVAS_V1_COMMANDS;}
unsigned pet_canvas_variable_limit(const pet_canvas_t *c){return c&&PET_CANVAS_IS_V2(c)?PET_CANVAS_VARS:PET_CANVAS_V1_VARS;}
unsigned pet_canvas_span_limit(const pet_canvas_t *c){return c&&PET_CANVAS_IS_V2(c)?PET_CANVAS_SPANS:PET_CANVAS_V1_SPANS;}
bool pet_canvas_valid(const pet_canvas_t *c){
  if(!c||!c->w||c->w>320||!c->h||c->h>240||c->x+c->w>640||c->y+c->h>480
      ||!c->count||c->count>pet_canvas_command_limit(c)||c->variable_count>pet_canvas_variable_limit(c)||c->page>4)return false;
#if PET_P4_CANVAS_V2
  if(c->point_count>PET_CANVAS_POINTS||(!c->v2&&c->point_count))return false;
  for(unsigned i=0;i<c->point_count;i++)
    if(!operand(c->points[i][0],c->w-1,c->variable_count,false)||!operand(c->points[i][1],c->h-1,c->variable_count,false))return false;
#endif
  unsigned cost=0,radius=(c->w<c->h?c->w:c->h)/2,budget=pet_canvas_span_limit(c);
  for(unsigned i=0;i<c->count;i++){
    const pet_canvas_command_t *p=&c->commands[i];
#if PET_P4_CANVAS_V2
    if(p->op==PET_CANVAS_POLYLINE){
      if(!c->v2||p->x||p->y||p->c||p->d||p->a<0||p->b<(int)PET_CANVAS_POLYLINE_MIN||p->b>(int)PET_CANVAS_POLYLINE_MAX||(unsigned)p->a+(unsigned)p->b>c->point_count)return false;
      cost+=(unsigned)(p->b-1)*(c->w>c->h?c->w:c->h);
      if(cost>budget)return false;
      continue;
    }
    if(p->op==PET_CANVAS_ARC||p->op==PET_CANVAS_RING){
      if(!c->v2||!operand(p->x,c->w-1,c->variable_count,false)||!operand(p->y,c->h-1,c->variable_count,false)
          ||!operand(p->a,radius,c->variable_count,true))return false;
      unsigned r=p->a<0?radius:(unsigned)p->a;
      if(p->op==PET_CANVAS_ARC){
        if(p->b||!operand(p->c,359,c->variable_count,false)||!operand(p->d,359,c->variable_count,false))return false;
        cost+=6*r+8;
      }else{
        /* Inner radius: constant below the outer constant, or a bound name clamped at draw time. */
        if(p->c||p->d||!operand(p->b,radius-1,c->variable_count,false)||(p->a>=0&&p->b>=0&&p->b>=p->a))return false;
        cost+=4*r+2;
      }
      if(cost>budget)return false;
      continue;
    }
    if(p->c||p->d)return false;
#endif
    if(!operand(p->x,c->w-1,c->variable_count,false)||!operand(p->y,c->h-1,c->variable_count,false))return false;
    if(p->op==PET_CANVAS_LINE){
      if(!operand(p->a,c->w-1,c->variable_count,false)||!operand(p->b,c->h-1,c->variable_count,false))return false;
      cost+=c->w>c->h?c->w:c->h;
    }else if(p->op==PET_CANVAS_RECT){
      if(!operand(p->a,c->w,c->variable_count,true)||!operand(p->b,c->h,c->variable_count,true))return false;
      cost++;
    }else if(p->op==PET_CANVAS_CIRCLE){
      if(!operand(p->a,radius,c->variable_count,true)||p->b)return false;
      cost+=2*(p->a<0?radius:(unsigned)p->a)+1;
    }else return false;
    if(cost>budget)return false;
  }
  return true;
}
static int resolve(const pet_canvas_t *c,int encoded,int max){
  int32_t value=encoded<0?c->values[-encoded-1]:encoded;
  return value<0?0:value>max?max:(int)value;
}
static unsigned square_root(unsigned n){
  unsigned root=0,bit=1U<<14;
  while(bit){unsigned candidate=root+bit;root>>=1;if(n>=candidate){n-=candidate;root+=bit;}bit>>=2;}
  return root;
}
static unsigned emit(const pet_canvas_t *c,pet_canvas_rect_fn rect,void *ctx,int x,int y,int w,int h,uint16_t color){
  if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
  if(x+w>c->w)w=c->w-x;
  if(y+h>c->h)h=c->h-y;
  if(w<=0||h<=0)return 0;
  rect(ctx,c->x+x,c->y+y,w,h,color);return 1;
}
/* Bounded Bresenham; both endpoints are drawn, fuel is the longer canvas side. */
static unsigned line(const pet_canvas_t *c,pet_canvas_rect_fn rect,void *ctx,int x,int y,int tx,int ty,uint16_t color){
  unsigned emitted=0;
  int dx=tx>x?tx-x:x-tx,dy=ty>y?ty-y:y-ty,sx=x<tx?1:-1,sy=y<ty?1:-1,error=dx-dy;
  unsigned limit=c->w>c->h?c->w:c->h;
  for(unsigned step=0;step<limit;step++){emitted+=emit(c,rect,ctx,x,y,1,1,color);if(x==tx&&y==ty)break;
    int twice=2*error;if(twice>-dy){error-=dy;x+=sx;}if(twice<dx){error+=dx;y+=sy;}}
  return emitted;
}
#if PET_P4_CANVAS_V2
unsigned pet_canvas_angle(int dx,int dy){
  unsigned ax=dx<0?(unsigned)-dx:(unsigned)dx,ay=dy<0?(unsigned)-dy:(unsigned)dy;
  if(!ax&&!ay)return 0;
  unsigned big=ax>ay?ax:ay,small=ax>ay?ay:ax;
  unsigned t=small*1024U/big,u=1024U-t;
  /* atan(t) in Q10 degrees: 45t + t(1-t)(14.02+3.80t); every term is nonnegative. */
  unsigned q=45U*t+((t*u/1024U)*(14356U+3891U*t/1024U))/1024U;
  unsigned a=q/1024U;
  if(ay>ax)a=90U-a;
  if(dx<0&&dy>=0)a=180U-a;else if(dx<0&&dy<0)a=180U+a;else if(dx>=0&&dy<0)a=360U-a;
  return a%360U;
}
static bool angle_in(unsigned a,unsigned s,unsigned e){return s<=e?(a>=s&&a<=e):(a>=s||a<=e);}
static unsigned arc_point(const pet_canvas_t *c,pet_canvas_rect_fn rect,void *ctx,int cx,int cy,int dx,int dy,unsigned s,unsigned e,uint16_t color){
  return angle_in(pet_canvas_angle(dx,dy),s,e)?emit(c,rect,ctx,cx+dx,cy+dy,1,1,color):0;
}
/* Midpoint circle outline: at most 6r+8 distinct points, each filtered by angle. */
static unsigned arc(const pet_canvas_t *c,pet_canvas_rect_fn rect,void *ctx,int cx,int cy,int r,unsigned s,unsigned e,uint16_t color){
  unsigned emitted=0;int x=r,y=0,error=1-r;
  while(x>=y){
    emitted+=arc_point(c,rect,ctx,cx,cy,x,y,s,e,color);
    emitted+=arc_point(c,rect,ctx,cx,cy,-x,y,s,e,color);
    if(y){emitted+=arc_point(c,rect,ctx,cx,cy,x,-y,s,e,color);emitted+=arc_point(c,rect,ctx,cx,cy,-x,-y,s,e,color);}
    if(x!=y){
      emitted+=arc_point(c,rect,ctx,cx,cy,y,x,s,e,color);emitted+=arc_point(c,rect,ctx,cx,cy,y,-x,s,e,color);
      if(y){emitted+=arc_point(c,rect,ctx,cx,cy,-y,x,s,e,color);emitted+=arc_point(c,rect,ctx,cx,cy,-y,-x,s,e,color);}
    }
    y++;if(error<0)error+=2*y+1;else{x--;error+=2*(y-x)+1;}
  }
  return emitted;
}
#endif
unsigned pet_canvas_draw(const pet_canvas_t *c,pet_canvas_rect_fn rect,void *ctx){
  if(!rect||!pet_canvas_valid(c))return 0;
  unsigned emitted=0;int radius=(c->w<c->h?c->w:c->h)/2;
  for(unsigned i=0;i<c->count;i++){
    const pet_canvas_command_t *p=&c->commands[i];
#if PET_P4_CANVAS_V2
    if(p->op==PET_CANVAS_POLYLINE){
      for(int j=p->a;j<p->a+p->b-1;j++){
        int x=resolve(c,c->points[j][0],c->w-1),y=resolve(c,c->points[j][1],c->h-1);
        int tx=resolve(c,c->points[j+1][0],c->w-1),ty=resolve(c,c->points[j+1][1],c->h-1);
        emitted+=line(c,rect,ctx,x,y,tx,ty,p->color);
      }
      continue;
    }
#endif
    int x=resolve(c,p->x,c->w-1),y=resolve(c,p->y,c->h-1);
    if(p->op==PET_CANVAS_RECT){
      emitted+=emit(c,rect,ctx,x,y,resolve(c,p->a,c->w),resolve(c,p->b,c->h),p->color);
    }else if(p->op==PET_CANVAS_LINE){
      emitted+=line(c,rect,ctx,x,y,resolve(c,p->a,c->w-1),resolve(c,p->b,c->h-1),p->color);
#if PET_P4_CANVAS_V2
    }else if(p->op==PET_CANVAS_ARC){
      int r=resolve(c,p->a,radius);
      if(!r)continue;
      emitted+=arc(c,rect,ctx,x,y,r,(unsigned)resolve(c,p->c,359),(unsigned)resolve(c,p->d,359),p->color);
    }else if(p->op==PET_CANVAS_RING){
      int r=resolve(c,p->a,radius);
      if(!r)continue;
      int inner=resolve(c,p->b,r-1);
      for(int row=-r;row<=r;row++){
        int half=(int)square_root((unsigned)(r*r-row*row)),ar=row<0?-row:row;
        if(ar<=inner){
          int hole=(int)square_root((unsigned)(inner*inner-row*row));
          emitted+=emit(c,rect,ctx,x-half,y+row,half-hole,1,p->color);
          emitted+=emit(c,rect,ctx,x+hole+1,y+row,half-hole,1,p->color);
        }else emitted+=emit(c,rect,ctx,x-half,y+row,2*half+1,1,p->color);
      }
#endif
    }else{
      int r=resolve(c,p->a,radius);
      if(!r)continue;
      for(int row=-r;row<=r;row++){
        int half=(int)square_root((unsigned)(r*r-row*row));
        emitted+=emit(c,rect,ctx,x-half,y+row,2*half+1,1,p->color);
      }
    }
  }
  return emitted;
}
