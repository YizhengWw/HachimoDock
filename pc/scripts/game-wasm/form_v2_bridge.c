/* Product-owned fixed-memory ABI. No filesystem, network or user program. */
#include "pet_p4_form_v2.h"
#include <stddef.h>
void *memset(void*p,int c,size_t n){unsigned char*s=p;while(n--)*s++=(unsigned char)c;return p;}
void *memcpy(void*p,const void*q,size_t n){unsigned char*d=p;const unsigned char*s=q;while(n--)*d++=*s++;return p;}
int memcmp(const void*p,const void*q,size_t n){const unsigned char*a=p,*b=q;while(n--){if(*a!=*b)return *a-*b;a++;b++;}return 0;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static pet_form_t form;
static pet_form_text_config_t text;
static pet_form_value_t values[8];
static char buffer[128];
static bool ready;
char *fv2_buffer(void){memset(buffer,0,sizeof(buffer));return buffer;}
static void copy(char *out,unsigned cap){unsigned n=0;while(n+1<cap&&buffer[n]){out[n]=buffer[n];n++;}out[n]=0;}
void fv2_begin(int count,int page,int focus,int active,int index,int label,int characters,int mask){
  memset(&form,0,sizeof(form));memset(&text,0,sizeof(text));memset(values,0,sizeof(values));ready=false;
  form.enabled=text.enabled=true;form.count=count;form.page=page;form.focus_var=focus;form.active_var=active;
  text.index_var=index;text.label_var=label;text.character_count=characters;text.text_mask=mask;
  for(unsigned i=0;i<8;i++){char *s=i<6?form.actions[i]:text.extra_actions[i-6];s[0]='a';s[1]=(char)('0'+i);s[2]=0;}
}
void fv2_field(unsigned i,int a,int b,int min,int max,int step,int max_bytes){
  if(i>=3)return;pet_form_field_t*f=&form.fields[i];f->id[0]='f';f->id[1]=(char)('0'+i);f->var=a;f->draft_var=b;f->min=min;f->max=max;f->step=step;text.max_bytes[i]=max_bytes;
}
void fv2_option(unsigned field,unsigned index,int value){
  if(field>=3||index>=8)return;pet_form_field_t*f=&form.fields[field];f->option_count=index+1;f->values[index]=value;copy(f->labels[index],64);
}
void fv2_character(unsigned index){if(index<96)copy(text.characters[index],5);}
void fv2_space(void){copy(text.space_label,17);}
int fv2_commit(void){ready=pet_form2_valid(&form,&text);return ready;}
void fv2_clear_values(void){memset(values,0,sizeof(values));}
void fv2_value(unsigned i,int value,int string){if(i<8){if(string)copy(values[i].text,64);else values[i].number=value;}}
pet_form_value_t *fv2_values(void){return values;}
int fv2_initialize(void){return ready&&pet_form2_initialize(&form,&text,values);}
int fv2_cancel(void){return ready&&pet_form2_cancel(&form,&text,values);}
int fv2_allowed(int page,int op){return ready&&pet_form2_allowed(&form,&text,values,page,op);}
int fv2_apply(int page,int op){return ready&&pet_form2_apply(&form,&text,values,page,op);}
char *fv2_display(int field){memset(buffer,0,sizeof(buffer));if(ready)pet_form2_text(&form,&text,values,field,buffer);return buffer;}
int fv2_replace(int page,int field){return ready?pet_form2_replace(&form,&text,values,page,field,buffer):PET_FORM_REPLACE_DENIED;}
