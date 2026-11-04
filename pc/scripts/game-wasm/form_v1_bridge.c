/* Product-owned legacy form ABI. Existing device C state machine,
 * no JS reimplementation, providers, package code or IO. Display labels stay
 * authored data; only native selected index/value crosses the fixed ABI.
 */
#include "pet_p4_app_form.h"
#include <stddef.h>
static pet_form_t form;
static int32_t values[PET_FORM_VARS];
static unsigned fields,option_masks[PET_FORM_FIELDS],expected_options[PET_FORM_FIELDS];
static bool ready,invalid;
static void clear(void *p,size_t n){unsigned char *b=p;for(size_t i=0;i<n;i++)b[i]=0;}
void f1_begin(unsigned count,unsigned page,unsigned focus,unsigned active){
  clear(&form,sizeof(form));clear(values,sizeof(values));clear(option_masks,sizeof(option_masks));clear(expected_options,sizeof(expected_options));
  fields=0;ready=false;invalid=count<1||count>PET_FORM_FIELDS||page>3||focus>=PET_FORM_VARS||active>=PET_FORM_VARS||focus==active;
  if(invalid)return;
  form.enabled=true;form.count=(uint8_t)count;form.page=(uint8_t)page;form.focus_var=(uint8_t)focus;form.active_var=(uint8_t)active;
}
void f1_field(unsigned index,unsigned value,unsigned draft,int32_t minimum,int32_t maximum,int32_t step,unsigned choices){
  if(ready){invalid=true;ready=false;return;}
  if(invalid||index>=form.count||value>=PET_FORM_VARS||draft>=PET_FORM_VARS||(fields&(1U<<index))||choices>8||choices==1){invalid=true;return;}
  if(choices?(minimum||maximum||step):(minimum< -1000000000||maximum>1000000000||minimum>=maximum||step<1||step>1000000000||((int64_t)maximum-minimum)%step)){
    invalid=true;return;
  }
  pet_form_field_t *f=&form.fields[index];f->var=(uint8_t)value;f->draft_var=(uint8_t)draft;
  f->min=minimum;f->max=maximum;f->step=step;f->option_count=(uint8_t)choices;
  expected_options[index]=choices;fields|=1U<<index;
}
void f1_option(unsigned field,unsigned index,int32_t value){
  if(ready){invalid=true;ready=false;return;}
  if(invalid||field>=form.count||!(fields&(1U<<field))||index>=expected_options[field]||value< -1000000000||value>1000000000||(option_masks[field]&(1U<<index))){invalid=true;return;}
  form.fields[field].values[index]=value;option_masks[field]|=1U<<index;
}
int f1_commit(void){
  if(invalid||!form.enabled||fields!=((1U<<form.count)-1U))return 0;
  unsigned used=(1U<<form.focus_var)|(1U<<form.active_var);
  for(unsigned i=0;i<form.count;i++){
    const pet_form_field_t *f=&form.fields[i];unsigned mask=(1U<<f->var)|(1U<<f->draft_var);
    if(f->var==f->draft_var||(used&mask)||option_masks[i]!=((1U<<expected_options[i])-1U))return 0;
    used|=mask;
    for(unsigned a=0;a<f->option_count;a++)for(unsigned b=0;b<a;b++)if(f->values[a]==f->values[b])return 0;
  }
  ready=true;return 1;
}
int32_t *f1_values(void){return values;}
int f1_initialize(void){if(!ready)return 0;bool changed=pet_form_cancel(&form,values);pet_form_initialize(&form,values);return changed;}
int f1_cancel(void){return ready&&pet_form_cancel(&form,values);}
int f1_allowed(int page,int op){return ready&&pet_form_allowed(&form,values,page,op);}
int f1_apply(int page,int op){return ready&&pet_form_apply(&form,values,page,op);}
int32_t f1_display_value(unsigned index){
  if(!ready||index>=form.count)return 0;
  const pet_form_field_t *f=&form.fields[index];return values[values[form.active_var]==1?f->draft_var:f->var];
}
int f1_choice_index(unsigned index){return ready&&index<form.count?pet_form_choice(&form.fields[index],f1_display_value(index)):-1;}
