/* Fixed product form-v3 ABI, synchronous complete snapshots; no imports. */
#include "pet_p4_form_v3.h"
#include <stddef.h>
void *memset(void*p,int c,size_t n){unsigned char*s=p;while(n--)*s++=(unsigned char)c;return p;}
void *memcpy(void*p,const void*q,size_t n){unsigned char*d=p;const unsigned char*s=q;while(n--)*d++=*s++;return p;}
int memcmp(const void*p,const void*q,size_t n){const unsigned char*a=p,*b=q;while(n--){if(*a!=*b)return *a-*b;a++;b++;}return 0;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
size_t strlen(const char*s){size_t n=0;while(s[n])n++;return n;}
static pet_form3_t form;
static pet_form3_value_t values[PET_FORM3_VARS];
static char buffer[128];static bool ready;
char *fv3_buffer(void){memset(buffer,0,sizeof(buffer));return buffer;}
static void copy(char*out,unsigned cap){unsigned n=0;while(n+1<cap&&buffer[n]){out[n]=buffer[n];n++;}out[n]=0;}
void fv3_begin(int count,int page,int focus,int active,int key,int cursor,int digits,int label,int candidates){
 form=(pet_form3_t){.enabled=true,.count=count,.page=page,.focus_var=focus,.active_var=active,.key_var=key,.cursor_var=cursor,.buffer_var=digits,.label_var=label,.candidates_var=candidates};ready=false;
 for(unsigned i=0;i<8;i++){form.actions[i][0]='a';form.actions[i][1]='0'+i;}
}
void fv3_field(unsigned i,int var,int draft,int mode,int bytes){if(i>=4)return;pet_form3_field_t*q=&form.fields[i];q->id[0]='f';q->id[1]='0'+i;q->var=var;q->draft_var=draft;q->mode=mode;q->max_bytes=bytes;}
int fv3_commit(void){ready=pet_form3_valid(&form);return ready;}
void fv3_clear_values(void){memset(values,0,sizeof(values));}
void fv3_value(unsigned i,int value,int string){if(i<PET_FORM3_VARS){if(string)copy(values[i].text,64);else values[i].number=value;}}
pet_form3_value_t *fv3_values(void){return values;}
int fv3_initialize(void){return ready&&pet_form3_initialize(&form,values);}
int fv3_cancel(void){return ready&&pet_form3_cancel(&form,values);}
int fv3_allowed(int page,int op){return ready&&pet_form3_allowed(&form,values,page,op);}
int fv3_apply(int page,int op){return ready&&pet_form3_apply(&form,values,page,op);}
char *fv3_display(int field){memset(buffer,0,sizeof(buffer));if(ready)pet_form3_text(&form,values,field,buffer);return buffer;}
int fv3_replace(int page,int field){return ready?pet_form3_replace(&form,values,page,field,buffer):PET_FORM_REPLACE_DENIED;}
char *strchr(const char*s,int c){do{if(*s==(char)c)return (char*)s;}while(*s++);return NULL;}
int strncmp(const char*a,const char*b,size_t n){while(n--){if(*a!=*b)return (unsigned char)*a-(unsigned char)*b;if(!*a)return 0;a++;b++;}return 0;}
