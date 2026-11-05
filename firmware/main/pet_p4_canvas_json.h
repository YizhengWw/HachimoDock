/* Strict data-only canvas decoder; command ownership stays with surface. */
#pragma once
#include "pet_p4_canvas.h"
#include "pet_p4_app_form_json.h"
static inline bool pet_canvas_number(const cJSON *o,const char *key,int lo,int hi,int16_t *out){
  int32_t value;if(!pet_form_integer(pet_form_get(o,key),&value)||value<lo||value>hi)return false;
  *out=(int16_t)value;return true;
}
static inline bool pet_canvas_operand(const cJSON *v,int lo,int hi,pet_canvas_t *c,int16_t *out){
  int32_t value;
  if(pet_form_integer(v,&value)){if(value<lo||value>hi)return false;*out=(int16_t)value;return true;}
  static const char *const keys[]={"var"};
  const char *name=pet_form_string(v,"var");
  if(!pet_form_keys(v,keys,1)||!pet_form_id(name,31))return false;
  for(unsigned i=0;i<c->variable_count;i++)if(!strcmp(name,c->names[i])){*out=-(int16_t)(i+1);return true;}
  if(c->variable_count==pet_canvas_variable_limit(c))return false;
  strcpy(c->names[c->variable_count++],name);*out=-(int16_t)c->variable_count;return true;
}
static inline bool pet_canvas_parse(const cJSON *j,pet_canvas_t *out){
  if(!out)return false;
  pet_canvas_t c={0};int16_t n;
  static const char *const root[]={"engine","x","y","w","h","commands","page"};
  const char *engine=pet_form_string(j,"engine");
  if(!pet_form_keys(j,root,pet_form_get(j,"page")?7:6))return false;
#if PET_P4_CANVAS_V2
  if(!strcmp(engine,"p4-canvas-v2"))c.v2=1;else
#endif
  if(strcmp(engine,"p4-canvas-v1"))return false;
  c.page=4;if(pet_form_get(j,"page")){if(!pet_canvas_number(j,"page",0,4,&n))return false;c.page=(uint8_t)n;}
  if(!pet_canvas_number(j,"x",0,639,&n))return false;
  c.x=n;
  if(!pet_canvas_number(j,"y",0,479,&n))return false;
  c.y=n;
  if(!pet_canvas_number(j,"w",1,320,&n))return false;
  c.w=n;
  if(!pet_canvas_number(j,"h",1,240,&n))return false;
  c.h=n;
  const cJSON *commands=pet_form_get(j,"commands");
  if(!cJSON_IsArray(commands)||!cJSON_GetArraySize(commands)||cJSON_GetArraySize(commands)>(int)pet_canvas_command_limit(&c))return false;
  c.count=cJSON_GetArraySize(commands);
  for(unsigned i=0;i<c.count;i++){
    const cJSON *v=cJSON_GetArrayItem(commands,i);pet_canvas_command_t *p=&c.commands[i];
    static const char *const line[]={"op","x","y","x2","y2","color"};
    static const char *const rectangle[]={"op","x","y","w","h","color"};
    static const char *const circle[]={"op","x","y","r","color"};
    const char *op=pet_form_string(v,"op");
    if(!strcmp(op,"line")&&pet_form_keys(v,line,6))p->op=PET_CANVAS_LINE;
    else if(!strcmp(op,"rect")&&pet_form_keys(v,rectangle,6))p->op=PET_CANVAS_RECT;
    else if(!strcmp(op,"circle")&&pet_form_keys(v,circle,5))p->op=PET_CANVAS_CIRCLE;
#if PET_P4_CANVAS_V2
    else if(c.v2){
      static const char *const polyline[]={"op","points","color"};
      static const char *const arc[]={"op","x","y","r","start","end","color"};
      static const char *const ring[]={"op","x","y","r","inner","color"};
      if(!strcmp(op,"polyline")&&pet_form_keys(v,polyline,3))p->op=PET_CANVAS_POLYLINE;
      else if(!strcmp(op,"arc")&&pet_form_keys(v,arc,7))p->op=PET_CANVAS_ARC;
      else if(!strcmp(op,"ring")&&pet_form_keys(v,ring,6))p->op=PET_CANVAS_RING;
      else return false;
    }
#endif
    else return false;
    int32_t color;if(!pet_form_integer(pet_form_get(v,"color"),&color)||color<0||color>65535)return false;p->color=(uint16_t)color;
#if PET_P4_CANVAS_V2
    if(p->op==PET_CANVAS_POLYLINE){
      const cJSON *points=pet_form_get(v,"points");int count=cJSON_GetArraySize(points);
      if(!cJSON_IsArray(points)||count<(int)PET_CANVAS_POLYLINE_MIN||count>(int)PET_CANVAS_POLYLINE_MAX||(unsigned)c.point_count+(unsigned)count>PET_CANVAS_POINTS)return false;
      p->a=(int16_t)c.point_count;p->b=(int16_t)count;
      for(int k=0;k<count;k++){
        const cJSON *pt=cJSON_GetArrayItem(points,k);
        if(!cJSON_IsArray(pt)||cJSON_GetArraySize(pt)!=2)return false;
        if(!pet_canvas_operand(cJSON_GetArrayItem(pt,0),0,c.w-1,&c,&c.points[c.point_count][0])
          ||!pet_canvas_operand(cJSON_GetArrayItem(pt,1),0,c.h-1,&c,&c.points[c.point_count][1]))return false;
        c.point_count++;
      }
      continue;
    }
#endif
    if(!pet_canvas_operand(pet_form_get(v,"x"),0,c.w-1,&c,&p->x)||!pet_canvas_operand(pet_form_get(v,"y"),0,c.h-1,&c,&p->y))return false;
    if(p->op==PET_CANVAS_CIRCLE){if(!pet_canvas_operand(pet_form_get(v,"r"),1,(c.w<c.h?c.w:c.h)/2,&c,&p->a))return false;}
#if PET_P4_CANVAS_V2
    else if(p->op==PET_CANVAS_ARC){
      if(!pet_canvas_operand(pet_form_get(v,"r"),1,(c.w<c.h?c.w:c.h)/2,&c,&p->a)
        ||!pet_canvas_operand(pet_form_get(v,"start"),0,359,&c,&p->c)||!pet_canvas_operand(pet_form_get(v,"end"),0,359,&c,&p->d))return false;
    }else if(p->op==PET_CANVAS_RING){
      if(!pet_canvas_operand(pet_form_get(v,"r"),1,(c.w<c.h?c.w:c.h)/2,&c,&p->a)
        ||!pet_canvas_operand(pet_form_get(v,"inner"),0,(c.w<c.h?c.w:c.h)/2-1,&c,&p->b)||(p->a>=0&&p->b>=0&&p->b>=p->a))return false;
    }
#endif
    else if(!pet_canvas_operand(pet_form_get(v,p->op==PET_CANVAS_LINE?"x2":"w"),p->op==PET_CANVAS_LINE?0:1,c.w-(p->op==PET_CANVAS_LINE),&c,&p->a)
        ||!pet_canvas_operand(pet_form_get(v,p->op==PET_CANVAS_LINE?"y2":"h"),p->op==PET_CANVAS_LINE?0:1,c.h-(p->op==PET_CANVAS_LINE),&c,&p->b))return false;
  }
  if(!pet_canvas_valid(&c))return false;
  *out=c;
  return true;
}
