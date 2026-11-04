#include <assert.h>
#include <limits.h>
#include <string.h>
#include "pet_p4_form_v2.h"
static pet_form_t form;
static pet_form_text_config_t config;
static void configure(void){
  memset(&form,0,sizeof(form));memset(&config,0,sizeof(config));
  form.enabled=true;form.count=2;form.focus_var=4;form.active_var=5;
  strcpy(form.fields[0].id,"title");form.fields[0].var=0;form.fields[0].draft_var=1;
  strcpy(form.fields[1].id,"amount");form.fields[1].var=2;form.fields[1].draft_var=3;
  form.fields[1].min=-1000000000;form.fields[1].max=1000000000;form.fields[1].step=1000000000;
  for(unsigned i=0;i<6;i++){strcpy(form.actions[i],"edit.0");form.actions[i][5]=(char)('0'+i);}
  config.enabled=true;config.text_mask=1;config.max_bytes[0]=10;config.index_var=6;config.label_var=7;
  config.character_count=4;strcpy(config.characters[0],"a");strcpy(config.characters[1],"中");
  strcpy(config.characters[2],"😀");strcpy(config.characters[3]," ");strcpy(config.space_label,"Space");
  strcpy(config.extra_actions[0],"append");strcpy(config.extra_actions[1],"erase");
  assert(pet_form2_valid(&form,&config));
}
static void denied(pet_form_value_t *v,int page,int op){
  pet_form_value_t before[8];memcpy(before,v,sizeof(before));
  assert(!pet_form2_allowed(&form,&config,v,page,op));assert(!pet_form2_apply(&form,&config,v,page,op));assert(!memcmp(before,v,sizeof(before)));
}
int main(void){
  unsigned n=77,c=88,last=99;
  assert(pet_form_utf8("a中😀",8,&n,&c,&last)&&n==8&&c==3&&last==4);
  assert(!pet_form_utf8("a中😀",7,&n,&c,&last)&&n==8&&c==3&&last==4);
  const char *bad[]={"\xc0\xaf","\xe0\x80\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\x80","\xc2\x85","\x7f","\x1f","\xf0\x9f"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!pet_form_utf8(bad[i],63,NULL,NULL,NULL));
  char bounded[64];memset(bounded,'x',sizeof(bounded));assert(!pet_form_utf8(bounded,63,NULL,NULL,NULL));
  configure();pet_form_value_t v[8]={0};strcpy(v[0].text,"中");v[2].number=0;
  assert(pet_form2_initialize(&form,&config,v));assert(!strcmp(v[1].text,"中")&&!strcmp(v[7].text,"a"));
  denied(v,1,PET_FORM_ACCEPT);denied(v,0,PET_FORM_APPEND);denied(v,0,8);denied(v,0,-1);
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ACCEPT));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_APPEND));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_INCREASE));assert(pet_form2_apply(&form,&config,v,0,PET_FORM_INCREASE));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_APPEND));assert(!strcmp(v[1].text,"中a😀"));
  denied(v,0,PET_FORM_APPEND);assert(!strcmp(v[0].text,"中"));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ERASE)&&!strcmp(v[1].text,"中a"));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_INCREASE)&&!strcmp(v[7].text,"Space"));
  denied(v,0,PET_FORM_INCREASE);assert(pet_form2_apply(&form,&config,v,0,PET_FORM_APPEND));assert(!strcmp(v[1].text,"中a "));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_NEXT));denied(v,0,PET_FORM_APPEND);
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_INCREASE)&&v[3].number==1000000000);denied(v,0,PET_FORM_INCREASE);
  v[3].number=1;denied(v,0,PET_FORM_ACCEPT);assert(v[2].number==0&&!strcmp(v[0].text,"中"));v[3].number=1000000000;
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ACCEPT)&&v[2].number==1000000000&&!strcmp(v[0].text,"中a "));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_PREVIOUS));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ACCEPT));
  assert(pet_form2_replace(&form,&config,v,0,0,"替换😀")==PET_FORM_REPLACE_OK);
  assert(pet_form2_replace(&form,&config,v,0,0,"替换😀")==PET_FORM_REPLACE_UNCHANGED);
  assert(pet_form2_replace(&form,&config,v,0,0,"超过十字节")==PET_FORM_REPLACE_LIMIT);
  assert(pet_form2_replace(&form,&config,v,0,0,"\xed\xa0\x80")==PET_FORM_REPLACE_INVALID);
  assert(pet_form2_replace(&form,&config,v,1,0,"ok")==PET_FORM_REPLACE_DENIED);
  assert(pet_form2_replace(&form,&config,v,0,1,"ok")==PET_FORM_REPLACE_DENIED);
  assert(!strcmp(v[1].text,"替换😀")&&!strcmp(v[0].text,"中a "));
  assert(pet_form2_cancel(&form,&config,v)&&!strcmp(v[1].text,v[0].text));assert(!pet_form2_cancel(&form,&config,v));
  /* Narrowing a text budget cannot clear or truncate existing committed data. */
  strcpy(v[0].text,"已保存的旧值");assert(pet_form2_initialize(&form,&config,v));
  assert(!strcmp(v[0].text,"已保存的旧值")&&!strcmp(v[1].text,v[0].text));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ACCEPT));denied(v,0,PET_FORM_ACCEPT);
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ERASE));assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ERASE));
  denied(v,0,PET_FORM_ACCEPT);assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ERASE));
  assert(pet_form2_apply(&form,&config,v,0,PET_FORM_ACCEPT)&&!strcmp(v[0].text,"已保存"));
  char display[121];assert(pet_form2_text(&form,&config,v,0,display)&&!strcmp(display,"已保存"));
  v[2].number=INT32_MIN;assert(pet_form2_text(&form,&config,v,1,display)&&!strcmp(display,"-2147483648"));
  /* Hostile metadata fails validation rather than acquiring more ownership. */
  config.index_var=4;assert(!pet_form2_valid(&form,&config));configure();strcpy(config.characters[1],"a");assert(!pet_form2_valid(&form,&config));
  configure();memset(config.characters[0],'x',5);assert(!pet_form2_valid(&form,&config));
  pet_form_value_t old[8];memcpy(old,v,sizeof(old));assert(!pet_form2_initialize(&form,&config,v));assert(!memcmp(old,v,sizeof(old)));
  configure();strcpy(form.fields[1].id,"title");assert(!pet_form2_valid(&form,&config));
  /* No allocator and no unbounded growth even across many edit/cancel cycles. */
  configure();memset(v,0,sizeof(v));assert(pet_form2_initialize(&form,&config,v));
  uint32_t seed=7;for(unsigned i=0;i<20000;i++){seed=seed*1664525U+1013904223U;pet_form2_apply(&form,&config,v,0,(int)((seed>>16)%8));assert(pet_form_utf8(v[0].text,63,NULL,NULL,NULL));assert(pet_form_utf8(v[1].text,63,NULL,NULL,NULL));}
  return 0;
}
