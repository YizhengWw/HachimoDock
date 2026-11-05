#include "pet_p4_form_v2.h"

bool pet_form_utf8(const char *s,unsigned cap,unsigned *length,unsigned *scalars,unsigned *last){
  if(!s||cap>63)return false;
  unsigned i=0,count=0,previous=0;
  while(i<=cap){
    unsigned char first=(unsigned char)s[i];if(!first){if(length)*length=i;if(scalars)*scalars=count;if(last)*last=previous;return true;}
    if(i==cap)return false;
    unsigned n=first<128?1:first>=0xc2&&first<=0xdf?2:first>=0xe0&&first<=0xef?3:first>=0xf0&&first<=0xf4?4:0;
    if(!n||n>cap-i)return false;
    uint32_t cp=first&(n==1?127:n==2?31:n==3?15:7);
    for(unsigned j=1;j<n;j++){unsigned char c=(unsigned char)s[i+j];if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&63);}
    if(cp<32||(cp>=127&&cp<160)||(n==2&&cp<128)||(n==3&&cp<2048)||(n==4&&cp<65536)
        ||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
    previous=i;i+=n;count++;
  }
  return false;
}
static bool name(const char *s,unsigned cap){
  if(!s||!s[0])return false;
  for(unsigned i=0;i<=cap;i++){unsigned char c=(unsigned char)s[i];if(!c)return true;if(i==cap||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;}
  return false;
}
static bool shape(const pet_form_t *f,const pet_form_text_config_t *t){
  if(!f||!t||!f->enabled||!t->enabled||!f->count||f->count>PET_FORM_FIELDS||f->page>3
      ||!t->text_mask||(t->text_mask>>f->count)||!t->character_count||t->character_count>PET_FORM_CHARACTERS)return false;
  unsigned mask=0;uint8_t roles[4]={f->focus_var,f->active_var,t->index_var,t->label_var};
  for(unsigned i=0;i<4;i++){if(roles[i]>=PET_FORM_VARS||(mask&(1U<<roles[i])))return false;mask|=1U<<roles[i];}
  for(unsigned i=0;i<f->count;i++){
    const pet_form_field_t *q=&f->fields[i];
    if(!name(q->id,15))return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(q->id,f->fields[j].id))return false;
    if(q->var>=PET_FORM_VARS||q->draft_var>=PET_FORM_VARS||q->var==q->draft_var
        ||(mask&((1U<<q->var)|(1U<<q->draft_var))))return false;
    mask|=(1U<<q->var)|(1U<<q->draft_var);
    if((t->text_mask&(1U<<i))&&(!t->max_bytes[i]||t->max_bytes[i]>63))return false;
  }
  return true;
}
bool pet_form2_valid(const pet_form_t *f,const pet_form_text_config_t *t){
  if(!shape(f,t))return false;
  unsigned length,count;
  if(!pet_form_utf8(t->space_label,16,&length,NULL,NULL)||!length)return false;
  for(unsigned i=0;i<t->character_count;i++){
    if(!pet_form_utf8(t->characters[i],4,NULL,&count,NULL)||count!=1)return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(t->characters[i],t->characters[j]))return false;
  }
  for(unsigned i=0;i<PET_FORM_V2_ACTIONS;i++){
    const char *a=i<6?f->actions[i]:t->extra_actions[i-6];if(!name(a,47))return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(a,j<6?f->actions[j]:t->extra_actions[j-6]))return false;
  }
  for(unsigned i=0;i<f->count;i++)if(!(t->text_mask&(1U<<i))){
    const pet_form_field_t *q=&f->fields[i];
    if(q->option_count){if(q->option_count<2||q->option_count>PET_FORM_OPTIONS)return false;
      for(unsigned a=0;a<q->option_count;a++){if(q->values[a]<-1000000000||q->values[a]>1000000000||!pet_form_utf8(q->labels[a],63,&length,NULL,NULL)||!length)return false;for(unsigned b=0;b<a;b++)if(q->values[a]==q->values[b])return false;}
    }else if(q->min<-1000000000||q->max>1000000000||q->min>=q->max||q->step<=0||q->step>1000000000||((int64_t)q->max-q->min)%q->step)return false;
  }
  return true;
}
int pet_form2_operation(const pet_form_t *f,const pet_form_text_config_t *t,const char *action){
  if(!shape(f,t)||!name(action,47))return -1;
  for(unsigned i=0;i<PET_FORM_V2_ACTIONS;i++){const char *a=i<6?f->actions[i]:t->extra_actions[i-6];if(!name(a,47))return -1;if(!strcmp(a,action))return (int)i;}
  return -1;
}
static void copy_text(char out[64],const char *s){unsigned i=0;for(;i<63&&s[i];i++)out[i]=s[i];out[i]=0;for(i++;i<64;i++)out[i]=0;}
static bool committed(const pet_form_t *f,const pet_form_text_config_t *t,const pet_form_value_t v[PET_FORM_VARS]){
  if(!v||!shape(f,t))return false;
  for(unsigned i=0;i<f->count;i++)if((t->text_mask&(1U<<i))&&!pet_form_utf8(v[f->fields[i].var].text,63,NULL,NULL,NULL))return false;
  return true;
}
static bool state_valid(const pet_form_t *f,const pet_form_text_config_t *t,const pet_form_value_t v[PET_FORM_VARS]){
  if(!committed(f,t,v)||v[f->focus_var].number<0||v[f->focus_var].number>=f->count
      ||v[f->active_var].number<0||v[f->active_var].number>1||v[t->index_var].number<0||v[t->index_var].number>=t->character_count)return false;
  for(unsigned i=0;i<f->count;i++)if((t->text_mask&(1U<<i))&&!pet_form_utf8(v[f->fields[i].draft_var].text,63,NULL,NULL,NULL))return false;
  unsigned n;return pet_form_utf8(t->characters[v[t->index_var].number],4,NULL,&n,NULL)&&n==1&&pet_form_utf8(t->space_label,16,NULL,NULL,NULL);
}
static void label(const pet_form_text_config_t *t,pet_form_value_t v[PET_FORM_VARS]){
  const char *key=t->characters[v[t->index_var].number];copy_text(v[t->label_var].text,!strcmp(key," ")?t->space_label:key);
}
static bool reset(const pet_form_t *f,const pet_form_text_config_t *t,pet_form_value_t v[PET_FORM_VARS]){
  if(!committed(f,t,v))return false;
  for(unsigned i=0;i<f->count;i++){const pet_form_field_t *q=&f->fields[i];
    if(t->text_mask&(1U<<i))copy_text(v[q->draft_var].text,v[q->var].text);else v[q->draft_var].number=v[q->var].number;}
  v[f->active_var].number=0;return true;
}
bool pet_form2_initialize(const pet_form_t *f,const pet_form_text_config_t *t,pet_form_value_t v[PET_FORM_VARS]){
  unsigned count;
  if(!committed(f,t,v)||!pet_form_utf8(t->characters[0],4,NULL,&count,NULL)||count!=1||!pet_form_utf8(t->space_label,16,NULL,NULL,NULL))return false;
  pet_form_value_t next[PET_FORM_VARS];memcpy(next,v,sizeof(next));
  if(!reset(f,t,next))return false;
  next[f->focus_var].number=0;
  next[t->index_var].number=0;
  label(t,next);
  memcpy(v,next,sizeof(next));
  return true;
}
bool pet_form2_cancel(const pet_form_t *f,const pet_form_text_config_t *t,pet_form_value_t v[PET_FORM_VARS]){
  if(!committed(f,t,v))return false;
  pet_form_value_t next[PET_FORM_VARS];
  memcpy(next,v,sizeof(next));
  if(!reset(f,t,next))return false;
  bool changed=memcmp(v,next,sizeof(next))!=0;
  if(changed)memcpy(v,next,sizeof(next));
  return changed;
}
static bool field_valid(const pet_form_t *f,const pet_form_text_config_t *t,const pet_form_value_t v[PET_FORM_VARS],unsigned i){
  const pet_form_field_t *q=&f->fields[i];return t->text_mask&(1U<<i)?pet_form_utf8(v[q->draft_var].text,t->max_bytes[i],NULL,NULL,NULL):pet_form_value_valid(q,v[q->draft_var].number);
}
bool pet_form2_allowed(const pet_form_t *f,const pet_form_text_config_t *t,const pet_form_value_t v[PET_FORM_VARS],int page,int op){
  if(!state_valid(f,t,v)||page!=f->page||op<0||op>=PET_FORM_V2_ACTIONS)return false;
  int focus=v[f->focus_var].number;const pet_form_field_t *q=&f->fields[focus];bool active=v[f->active_var].number==1;
  if(op==PET_FORM_PREVIOUS)return focus>0;
  if(op==PET_FORM_NEXT)return focus+1<f->count;
  if(op==PET_FORM_CANCEL)return active;
  if(op==PET_FORM_ACCEPT){if(!active)return true;for(unsigned i=0;i<f->count;i++)if(!field_valid(f,t,v,i))return false;return true;}
  if(!active)return false;
  if(t->text_mask&(1U<<focus)){
    int index=v[t->index_var].number;if(op==PET_FORM_DECREASE)return index>0;if(op==PET_FORM_INCREASE)return index+1<t->character_count;
    unsigned bytes,key;if(!pet_form_utf8(v[q->draft_var].text,63,&bytes,NULL,NULL)||!pet_form_utf8(t->characters[index],4,&key,NULL,NULL))return false;
    return op==PET_FORM_APPEND?bytes+key<=t->max_bytes[focus]:op==PET_FORM_ERASE&&bytes>0;
  }
  if(op>=PET_FORM_APPEND||!field_valid(f,t,v,(unsigned)focus))return false;
  int32_t value=v[q->draft_var].number;int choice=pet_form_choice(q,value);
  return op==PET_FORM_DECREASE?(q->option_count?choice>0:value>q->min):(q->option_count?choice+1<q->option_count:value<q->max);
}
bool pet_form2_apply(const pet_form_t *f,const pet_form_text_config_t *t,pet_form_value_t v[PET_FORM_VARS],int page,int op){
  if(!pet_form2_allowed(f,t,v,page,op))return false;
  pet_form_value_t next[PET_FORM_VARS];memcpy(next,v,sizeof(next));int focus=next[f->focus_var].number;const pet_form_field_t *q=&f->fields[focus];
  if(op==PET_FORM_PREVIOUS||op==PET_FORM_NEXT)next[f->focus_var].number+=op==PET_FORM_NEXT?1:-1;
  else if(op==PET_FORM_CANCEL){if(!reset(f,t,next))return false;}
  else if(op==PET_FORM_ACCEPT){
    if(next[f->active_var].number==1){for(unsigned i=0;i<f->count;i++){const pet_form_field_t *p=&f->fields[i];if(t->text_mask&(1U<<i))copy_text(next[p->var].text,next[p->draft_var].text);else next[p->var].number=next[p->draft_var].number;}next[f->active_var].number=0;}
    else{for(unsigned i=0;i<f->count;i++){const pet_form_field_t *p=&f->fields[i];if(t->text_mask&(1U<<i))copy_text(next[p->draft_var].text,next[p->var].text);else{int32_t n=next[p->var].number;next[p->draft_var].number=pet_form_value_valid(p,n)?n:p->option_count?p->values[0]:p->min;}}next[f->active_var].number=1;}
  }else if(t->text_mask&(1U<<focus)){
    if(op==PET_FORM_DECREASE||op==PET_FORM_INCREASE){next[t->index_var].number+=op==PET_FORM_INCREASE?1:-1;label(t,next);}
    else{unsigned bytes,last,key;pet_form_utf8(next[q->draft_var].text,63,&bytes,NULL,&last);
      if(op==PET_FORM_ERASE)next[q->draft_var].text[last]=0;
      else{const char *s=t->characters[next[t->index_var].number];pet_form_utf8(s,4,&key,NULL,NULL);memcpy(next[q->draft_var].text+bytes,s,key+1);}}
  }else if(q->option_count){int index=pet_form_choice(q,next[q->draft_var].number)+(op==PET_FORM_INCREASE?1:-1);next[q->draft_var].number=q->values[index];}
  else next[q->draft_var].number=(int32_t)((int64_t)next[q->draft_var].number+(op==PET_FORM_INCREASE?1:-1)*(int64_t)q->step);
  memcpy(v,next,sizeof(next));return true;
}
bool pet_form2_text(const pet_form_t *f,const pet_form_text_config_t *t,const pet_form_value_t v[PET_FORM_VARS],int field,char out[121]){
  if(!out||!state_valid(f,t,v)||field<0||field>=f->count)return false;
  const pet_form_field_t *q=&f->fields[field];unsigned index=v[f->active_var].number==1?q->draft_var:q->var;
  if(t->text_mask&(1U<<field)){copy_text(out,v[index].text);return true;}
  int32_t n=v[index].number;if(q->option_count){int at=pet_form_choice(q,n);copy_text(out,at<0?"—":q->labels[at]);return true;}
  char buffer[12];unsigned count=0;int64_t value=n;if(value<0)value=-value;
  do{buffer[count++]=(char)('0'+value%10);value/=10;}while(value&&count<11);
  unsigned pos=0;if(n<0)out[pos++]='-';while(count)out[pos++]=buffer[--count];out[pos]=0;return true;
}
pet_form_replace_result_t pet_form2_replace(const pet_form_t *f,const pet_form_text_config_t *t,pet_form_value_t v[PET_FORM_VARS],int page,int field,const char *s){
  if(!state_valid(f,t,v)||page!=f->page||v[f->active_var].number!=1||field<0||field>=f->count
      ||field!=v[f->focus_var].number||!(t->text_mask&(1U<<field)))return PET_FORM_REPLACE_DENIED;
  unsigned bytes;if(!pet_form_utf8(s,63,&bytes,NULL,NULL))return PET_FORM_REPLACE_INVALID;
  if(bytes>t->max_bytes[field])return PET_FORM_REPLACE_LIMIT;
  const pet_form_field_t *q=&f->fields[field];if(!strcmp(s,v[q->draft_var].text))return PET_FORM_REPLACE_UNCHANGED;
  pet_form_value_t next[PET_FORM_VARS];memcpy(next,v,sizeof(next));copy_text(next[q->draft_var].text,s);memcpy(v,next,sizeof(next));return PET_FORM_REPLACE_OK;
}
