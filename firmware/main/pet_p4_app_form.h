/* Bounded local integer/choice editor. No allocation, persistence or input I/O.
 * Committed and draft values are distinct declared vars; accept is atomic. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_FORM_FIELDS 3
#define PET_FORM_OPTIONS 8
#define PET_FORM_VARS 8
#define PET_FORM_ACTIONS 6
typedef enum {PET_FORM_PREVIOUS,PET_FORM_NEXT,PET_FORM_DECREASE,PET_FORM_INCREASE,PET_FORM_ACCEPT,PET_FORM_CANCEL} pet_form_op_t;
typedef struct {
  char id[16];
  uint8_t var,draft_var,option_count;
  int32_t min,max,step,values[PET_FORM_OPTIONS];
  char labels[PET_FORM_OPTIONS][64];
} pet_form_field_t;
typedef struct {
  bool enabled;
  uint8_t count,page,focus_var,active_var;
  char actions[PET_FORM_ACTIONS][48];
  pet_form_field_t fields[PET_FORM_FIELDS];
} pet_form_t;
static inline int pet_form_field_index(const pet_form_t *form,const char *id){
  if(!form||!form->enabled||!id)return -1;
  for(int i=0;i<form->count;i++)if(!strcmp(form->fields[i].id,id))return i;
  return -1;
}
static inline int pet_form_operation(const pet_form_t *form,const char *action){
  if(!form||!form->enabled||!action)return -1;
  for(int i=0;i<PET_FORM_ACTIONS;i++)if(!strcmp(form->actions[i],action))return i;
  return -1;
}
static inline int pet_form_choice(const pet_form_field_t *field,int32_t value){
  for(int i=0;i<field->option_count;i++)if(field->values[i]==value)return i;
  return -1;
}
static inline bool pet_form_value_valid(const pet_form_field_t *field,int32_t value){
  return field->option_count?pet_form_choice(field,value)>=0:
    value>=field->min&&value<=field->max&&field->step>0&&((int64_t)value-field->min)%field->step==0;
}
static inline int pet_form_focus(const pet_form_t *form,const int32_t vars[PET_FORM_VARS]){
  int32_t value=vars[form->focus_var];return value>=0&&value<form->count?(int)value:0;
}
static inline bool pet_form_cancel(const pet_form_t *form,int32_t vars[PET_FORM_VARS]){
  if(!form||!form->enabled)return false;
  bool changed=vars[form->active_var]!=0;vars[form->active_var]=0;
  for(int i=0;i<form->count;i++){
    const pet_form_field_t *field=&form->fields[i];
    changed|=vars[field->draft_var]!=vars[field->var];vars[field->draft_var]=vars[field->var];
  }
  return changed;
}
static inline void pet_form_initialize(const pet_form_t *form,int32_t vars[PET_FORM_VARS]){
  if(form&&form->enabled){pet_form_cancel(form,vars);vars[form->focus_var]=0;}
}
static inline bool pet_form_allowed(const pet_form_t *form,const int32_t vars[PET_FORM_VARS],int page,int op){
  if(!form||!form->enabled||page!=form->page||op<0||op>=PET_FORM_ACTIONS)return false;
  int focus=pet_form_focus(form,vars);bool active=vars[form->active_var]==1;
  if(op==PET_FORM_PREVIOUS)return focus>0;
  if(op==PET_FORM_NEXT)return focus+1<form->count;
  if(op==PET_FORM_CANCEL)return active;
  if(op==PET_FORM_ACCEPT){
    if(!active)return true;
    for(int i=0;i<form->count;i++)if(!pet_form_value_valid(&form->fields[i],vars[form->fields[i].draft_var]))return false;
    return true;
  }
  if(!active)return false;
  const pet_form_field_t *field=&form->fields[focus];int32_t value=vars[field->draft_var];
  if(!pet_form_value_valid(field,value))return false;
  int choice=pet_form_choice(field,value);
  return op==PET_FORM_DECREASE?(field->option_count?choice>0:value>field->min):
    (field->option_count?choice+1<field->option_count:value<field->max);
}
static inline bool pet_form_apply(const pet_form_t *form,int32_t vars[PET_FORM_VARS],int page,int op){
  if(!pet_form_allowed(form,vars,page,op))return false;
  int focus=pet_form_focus(form,vars);
  if(op==PET_FORM_PREVIOUS||op==PET_FORM_NEXT){vars[form->focus_var]=focus+(op==PET_FORM_NEXT?1:-1);return true;}
  if(op==PET_FORM_CANCEL)return pet_form_cancel(form,vars);
  if(op==PET_FORM_ACCEPT){
    if(vars[form->active_var]==1){
      /* Validate all drafts above before changing even one committed value. */
      for(int i=0;i<form->count;i++)vars[form->fields[i].var]=vars[form->fields[i].draft_var];
      vars[form->active_var]=0;
    }else{
      for(int i=0;i<form->count;i++){
        const pet_form_field_t *field=&form->fields[i];int32_t value=vars[field->var];
        vars[field->draft_var]=pet_form_value_valid(field,value)?value:field->option_count?field->values[0]:field->min;
      }
      vars[form->active_var]=1;
    }
    return true;
  }
  const pet_form_field_t *field=&form->fields[focus];int direction=op==PET_FORM_INCREASE?1:-1;
  vars[field->draft_var]=field->option_count?field->values[pet_form_choice(field,vars[field->draft_var])+direction]:
    (int32_t)((int64_t)vars[field->draft_var]+(int64_t)direction*field->step);
  return true;
}
static inline void pet_form_text(const pet_form_t *form,const int32_t vars[PET_FORM_VARS],int field_index,char out[121]){
  out[0]=0;if(!form||!form->enabled||field_index<0||field_index>=form->count)return;
  const pet_form_field_t *field=&form->fields[field_index];int32_t value=vars[vars[form->active_var]==1?field->draft_var:field->var];
  if(field->option_count){int choice=pet_form_choice(field,value);snprintf(out,121,"%s",choice>=0?field->labels[choice]:"—");}
  else snprintf(out,121,"%ld",(long)value);
}
