#include "pet_p4_form_v3.h"
#include "pet_p4_pinyin_table.h"
#include <stdio.h>
const char pet_form3_numeric_keys[PET_FORM3_NUMERIC_KEYS+1]="0123456789.-";
const char *const pet_form3_groups[PET_FORM3_GROUPS]={" 0","1.-_@","abc2","def3","ghi4","jkl5","mno6","pqrs7","tuv8","wxyz9"};
static bool name(const char *s,unsigned cap){
  if(!s||!s[0])return false;
  for(unsigned i=0;i<=cap;i++){unsigned char c=(unsigned char)s[i];if(!c)return true;if(i==cap||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;}
  return false;
}
int pet_form3_mode(const char *n){
  if(!n)return -1;if(!strcmp(n,"numeric"))return PET_FORM3_NUMERIC;if(!strcmp(n,"latin"))return PET_FORM3_LATIN;if(!strcmp(n,"pinyin"))return PET_FORM3_PINYIN;return -1;
}
bool pet_form3_valid(const pet_form3_t *f){
  if(!f||!f->enabled||!f->count||f->count>PET_FORM3_FIELDS||f->page>=12)return false;
  unsigned mask=0;const uint8_t roles[7]={f->focus_var,f->active_var,f->key_var,f->cursor_var,f->buffer_var,f->label_var,f->candidates_var};
  for(unsigned i=0;i<7;i++){if(roles[i]>=PET_FORM3_VARS||(mask&(1U<<roles[i])))return false;mask|=1U<<roles[i];}
  for(unsigned i=0;i<f->count;i++){
    const pet_form3_field_t *q=&f->fields[i];
    if(!name(q->id,15)||q->mode>PET_FORM3_PINYIN||!q->max_bytes||q->max_bytes>63)return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(q->id,f->fields[j].id))return false;
    if(q->var>=PET_FORM3_VARS||q->draft_var>=PET_FORM3_VARS||q->var==q->draft_var||(mask&((1U<<q->var)|(1U<<q->draft_var))))return false;
    mask|=(1U<<q->var)|(1U<<q->draft_var);
  }
  for(unsigned i=0;i<PET_FORM3_ACTIONS;i++){
    if(!name(f->actions[i],47))return false;
    for(unsigned j=0;j<i;j++)if(!strcmp(f->actions[i],f->actions[j]))return false;
  }
  return true;
}
int pet_form3_operation(const pet_form3_t *f,const char *action){
  if(!pet_form3_valid(f)||!name(action,47))return -1;
  for(unsigned i=0;i<PET_FORM3_ACTIONS;i++)if(!strcmp(f->actions[i],action))return (int)i;
  return -1;
}
int pet_form3_field_index(const pet_form3_t *f,const char *id){
  if(!f||!f->enabled||!id)return -1;
  for(int i=0;i<f->count;i++)if(!strcmp(f->fields[i].id,id))return i;
  return -1;
}
static char t9_digit(char c){
  static const char *const letters[10]={"","","abc","def","ghi","jkl","mno","pqrs","tuv","wxyz"};
  for(int d=2;d<10;d++)if(strchr(letters[d],c))return (char)('0'+d);
  return 0;
}
static void syllable_code(unsigned i,char out[8]){
  const char *s=pet_pinyin_syllables+pet_pinyin_syllable_text[i];unsigned n=0;
  for(;s[n]&&n<7;n++)out[n]=t9_digit(s[n]);out[n]=0;
}
static bool digits_ok(const char *digits){
  if(!digits)return false;unsigned n=0;
  for(;digits[n];n++)if(digits[n]<'2'||digits[n]>'9'||n>=PET_FORM3_BUFFER_DIGITS)return false;
  return n>0;
}
unsigned pet_form3_candidates(const char *digits,uint16_t *out,unsigned max){
  if(!digits_ok(digits)||!out||!max)return 0;
  unsigned count=0,len=strlen(digits);
  for(int pass=0;pass<2&&count<max;pass++){
    for(unsigned i=0;i<PET_PINYIN_SYLLABLES&&count<max;i++){
      char code[8];syllable_code(i,code);
      bool exact=!strcmp(code,digits),prefix=!exact&&!strncmp(code,digits,len);
      if(pass==0?!exact:!prefix)continue;
      for(unsigned k=pet_pinyin_syllable_offset[i];k<pet_pinyin_syllable_offset[i+1]&&count<max;k++){
        uint16_t cp=pet_pinyin_chars[k];bool seen=false;
        for(unsigned j=0;j<count;j++)if(out[j]==cp){seen=true;break;}
        if(!seen)out[count++]=cp;
      }
    }
  }
  return count;
}
void pet_form3_syllables(const char *digits,char *out,unsigned capacity){
  if(!out||!capacity)return;out[0]=0;if(!digits_ok(digits))return;
  unsigned used=0;
  for(unsigned i=0;i<PET_PINYIN_SYLLABLES;i++){
    char code[8];syllable_code(i,code);if(strcmp(code,digits))continue;
    const char *s=pet_pinyin_syllables+pet_pinyin_syllable_text[i];unsigned n=strlen(s);
    if(used+n+(used?1:0)>=capacity)break;
    if(used)out[used++]='|';memcpy(out+used,s,n);used+=n;out[used]=0;
  }
}
static void copy_text(char out[64],const char *s){unsigned i=0;for(;i<63&&s[i];i++)out[i]=s[i];out[i]=0;for(i++;i<64;i++)out[i]=0;}
static void encode(uint32_t cp,char out[5]){
  if(cp<0x80){out[0]=(char)cp;out[1]=0;}
  else if(cp<0x800){out[0]=(char)(0xc0|(cp>>6));out[1]=(char)(0x80|(cp&63));out[2]=0;}
  else{out[0]=(char)(0xe0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&63));out[2]=(char)(0x80|(cp&63));out[3]=0;}
}
static unsigned key_count(uint8_t mode){return mode==PET_FORM3_NUMERIC?PET_FORM3_NUMERIC_KEYS:mode==PET_FORM3_LATIN?PET_FORM3_GROUPS:10;}
static int32_t default_key(uint8_t mode){return mode==PET_FORM3_NUMERIC?0:2;}
static unsigned candidates_of(const pet_form3_value_t *buffer,uint16_t out[PET_FORM3_CANDIDATES_MAX]){return pet_form3_candidates(buffer->text,out,PET_FORM3_CANDIDATES_MAX);}
/* Derived display strings: label (key/group/syllables) and candidate page. */
static void refresh(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS]){
  int focus=v[f->focus_var].number;const pet_form3_field_t *q=&f->fields[focus];
  char label[64]={0},cands[64]={0};
  if(v[f->active_var].number==1){
    int32_t key=v[f->key_var].number;
    if(q->mode==PET_FORM3_NUMERIC){label[0]=pet_form3_numeric_keys[key];label[1]=0;}
    else if(q->mode==PET_FORM3_LATIN){
      const char *g=pet_form3_groups[key];unsigned n=0,cursor=(unsigned)v[f->cursor_var].number;
      for(unsigned i=0;g[i]&&n<60;i++){if(i==cursor)label[n++]='[';label[n++]=g[i]==' '?'_':g[i];if(i==cursor)label[n++]=']';}
      label[n]=0;
    }else{
      if(v[f->buffer_var].text[0]){
        pet_form3_syllables(v[f->buffer_var].text,label,sizeof(label));
        if(!label[0])copy_text(label,v[f->buffer_var].text);
        uint16_t list[PET_FORM3_CANDIDATES_MAX];unsigned count=candidates_of(&v[f->buffer_var],list);
        unsigned cursor=(unsigned)v[f->cursor_var].number,start=cursor/PET_FORM3_PAGE*PET_FORM3_PAGE,n=0;
        for(unsigned i=start;i<count&&i<start+PET_FORM3_PAGE;i++){
          char ch[5];encode(list[i],ch);unsigned len=strlen(ch);
          if(n+len+3>=sizeof(cands))break;
          if(n)cands[n++]=' ';if(i==cursor)cands[n++]='[';memcpy(cands+n,ch,len);n+=len;if(i==cursor)cands[n++]=']';
        }
        cands[n]=0;
      }else{const char *g=pet_form3_groups[key];copy_text(label,g);}
    }
  }
  copy_text(v[f->label_var].text,label);copy_text(v[f->candidates_var].text,cands);
}
static bool committed(const pet_form3_t *f,const pet_form3_value_t v[PET_FORM3_VARS]){
  if(!v||!pet_form3_valid(f))return false;
  for(unsigned i=0;i<f->count;i++)if(!pet_form_utf8(v[f->fields[i].var].text,63,NULL,NULL,NULL))return false;
  return true;
}
static bool state_valid(const pet_form3_t *f,const pet_form3_value_t v[PET_FORM3_VARS]){
  if(!committed(f,v)||v[f->focus_var].number<0||v[f->focus_var].number>=f->count||v[f->active_var].number<0||v[f->active_var].number>1)return false;
  const pet_form3_field_t *q=&f->fields[v[f->focus_var].number];
  int32_t key=v[f->key_var].number;
  if(key<0||key>=(int32_t)key_count(q->mode)||(q->mode==PET_FORM3_PINYIN&&key<2))return false;
  if(v[f->cursor_var].number<0||v[f->cursor_var].number>=PET_FORM3_CANDIDATES_MAX)return false;
  const char *buffer=v[f->buffer_var].text;
  if(buffer[0]&&(q->mode!=PET_FORM3_PINYIN||!digits_ok(buffer)))return false;
  for(unsigned i=0;i<f->count;i++)if(!pet_form_utf8(v[f->fields[i].draft_var].text,63,NULL,NULL,NULL))return false;
  return true;
}
static void reset_keyboard(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS]){
  const pet_form3_field_t *q=&f->fields[v[f->focus_var].number];
  v[f->key_var].number=default_key(q->mode);v[f->cursor_var].number=0;copy_text(v[f->buffer_var].text,"");
}
static bool reset(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS]){
  if(!committed(f,v))return false;
  for(unsigned i=0;i<f->count;i++)copy_text(v[f->fields[i].draft_var].text,v[f->fields[i].var].text);
  v[f->active_var].number=0;
  if(v[f->focus_var].number<0||v[f->focus_var].number>=f->count)v[f->focus_var].number=0;
  reset_keyboard(f,v);refresh(f,v);return true;
}
bool pet_form3_initialize(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS]){
  if(!committed(f,v))return false;
  pet_form3_value_t next[PET_FORM3_VARS];memcpy(next,v,sizeof(next));
  next[f->focus_var].number=0;if(!reset(f,next))return false;memcpy(v,next,sizeof(next));return true;
}
bool pet_form3_cancel(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS]){
  if(!committed(f,v))return false;pet_form3_value_t next[PET_FORM3_VARS];memcpy(next,v,sizeof(next));
  if(!reset(f,next))return false;bool changed=memcmp(v,next,sizeof(next))!=0;if(changed)memcpy(v,next,sizeof(next));return changed;
}
static bool field_valid(const pet_form3_t *f,const pet_form3_value_t v[PET_FORM3_VARS],unsigned i){
  return pet_form_utf8(v[f->fields[i].draft_var].text,f->fields[i].max_bytes,NULL,NULL,NULL);
}
static bool fits(const pet_form3_t *f,const pet_form3_value_t v[PET_FORM3_VARS],unsigned field,unsigned extra){
  unsigned bytes;return pet_form_utf8(v[f->fields[field].draft_var].text,63,&bytes,NULL,NULL)&&bytes+extra<=f->fields[field].max_bytes;
}
bool pet_form3_allowed(const pet_form3_t *f,const pet_form3_value_t v[PET_FORM3_VARS],int page,int op){
  if(!state_valid(f,v)||page!=f->page||op<0||op>=PET_FORM3_ACTIONS)return false;
  int focus=v[f->focus_var].number;const pet_form3_field_t *q=&f->fields[focus];bool active=v[f->active_var].number==1;
  const char *buffer=v[f->buffer_var].text;int32_t key=v[f->key_var].number,cursor=v[f->cursor_var].number;
  if(!active){
    if(op==PET_FORM_PREVIOUS)return focus>0;
    if(op==PET_FORM_NEXT)return focus+1<f->count;
    return op==PET_FORM_ACCEPT;
  }
  if(op==PET_FORM_CANCEL)return true;
  if(op==PET_FORM_DECREASE||op==PET_FORM_INCREASE)return true;
  if(op==PET_FORM_ACCEPT){
    if(q->mode==PET_FORM3_PINYIN&&buffer[0]){
      uint16_t list[PET_FORM3_CANDIDATES_MAX];unsigned count=candidates_of(&v[f->buffer_var],list);
      if(!count||cursor>=(int32_t)count)return false;char ch[5];encode(list[cursor],ch);return fits(f,v,(unsigned)focus,strlen(ch));
    }
    for(unsigned i=0;i<f->count;i++)if(!field_valid(f,v,i))return false;return true;
  }
  if(op==PET_FORM_PREVIOUS||op==PET_FORM_NEXT){
    if(q->mode==PET_FORM3_NUMERIC)return false;
    if(q->mode==PET_FORM3_LATIN){int n=(int)strlen(pet_form3_groups[key]);return op==PET_FORM_PREVIOUS?cursor>0:cursor+1<n;}
    if(!buffer[0])return false;
    uint16_t list[PET_FORM3_CANDIDATES_MAX];int count=(int)candidates_of(&v[f->buffer_var],list);
    return op==PET_FORM_PREVIOUS?cursor>0:cursor+1<count;
  }
  if(op==PET_FORM_APPEND){
    if(q->mode==PET_FORM3_NUMERIC)return fits(f,v,(unsigned)focus,1);
    if(q->mode==PET_FORM3_LATIN){if(cursor>=(int32_t)strlen(pet_form3_groups[key]))return false;return fits(f,v,(unsigned)focus,1);}
    if(strlen(buffer)>=PET_FORM3_BUFFER_DIGITS)return false;
    char next[PET_FORM3_BUFFER_DIGITS+2];unsigned len=(unsigned)strlen(buffer);memcpy(next,buffer,len);next[len]=(char)('0'+key);next[len+1]=0;
    uint16_t list[1];return pet_form3_candidates(next,list,1)>0;
  }
  if(op==PET_FORM_ERASE){
    if(q->mode==PET_FORM3_PINYIN&&buffer[0])return true;
    unsigned bytes;return pet_form_utf8(v[q->draft_var].text,63,&bytes,NULL,NULL)&&bytes>0;
  }
  return false;
}
bool pet_form3_apply(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS],int page,int op){
  if(!pet_form3_allowed(f,v,page,op))return false;
  pet_form3_value_t next[PET_FORM3_VARS];memcpy(next,v,sizeof(next));
  int focus=next[f->focus_var].number;const pet_form3_field_t *q=&f->fields[focus];bool active=next[f->active_var].number==1;
  char *buffer=next[f->buffer_var].text;
  if(!active){
    if(op==PET_FORM_PREVIOUS||op==PET_FORM_NEXT){next[f->focus_var].number+=op==PET_FORM_NEXT?1:-1;}
    else{for(unsigned i=0;i<f->count;i++)copy_text(next[f->fields[i].draft_var].text,next[f->fields[i].var].text);next[f->active_var].number=1;reset_keyboard(f,next);}
  }else if(op==PET_FORM_CANCEL){
    if(q->mode==PET_FORM3_PINYIN&&buffer[0]){copy_text(buffer,"");next[f->cursor_var].number=0;}
    else{if(!reset(f,next))return false;}
  }else if(op==PET_FORM_DECREASE||op==PET_FORM_INCREASE){
    int n=(int)key_count(q->mode),lo=q->mode==PET_FORM3_PINYIN?2:0,span=n-lo;
    int k=next[f->key_var].number-lo+(op==PET_FORM_INCREASE?1:span-1);next[f->key_var].number=lo+k%span;
    if(q->mode!=PET_FORM3_PINYIN)next[f->cursor_var].number=0;
  }else if(op==PET_FORM_PREVIOUS||op==PET_FORM_NEXT){
    next[f->cursor_var].number+=op==PET_FORM_NEXT?1:-1;
  }else if(op==PET_FORM_APPEND){
    char *draft=next[q->draft_var].text;unsigned bytes;pet_form_utf8(draft,63,&bytes,NULL,NULL);
    if(q->mode==PET_FORM3_NUMERIC){draft[bytes]=pet_form3_numeric_keys[next[f->key_var].number];draft[bytes+1]=0;}
    else if(q->mode==PET_FORM3_LATIN){draft[bytes]=pet_form3_groups[next[f->key_var].number][next[f->cursor_var].number];draft[bytes+1]=0;}
    else{unsigned n=strlen(buffer);buffer[n]=(char)('0'+next[f->key_var].number);buffer[n+1]=0;next[f->cursor_var].number=0;}
  }else if(op==PET_FORM_ERASE){
    if(q->mode==PET_FORM3_PINYIN&&buffer[0]){buffer[strlen(buffer)-1]=0;next[f->cursor_var].number=0;}
    else{unsigned last;pet_form_utf8(next[q->draft_var].text,63,NULL,NULL,&last);next[q->draft_var].text[last]=0;}
  }else if(op==PET_FORM_ACCEPT){
    if(q->mode==PET_FORM3_PINYIN&&buffer[0]){
      uint16_t list[PET_FORM3_CANDIDATES_MAX];candidates_of(&next[f->buffer_var],list);
      char ch[5];encode(list[next[f->cursor_var].number],ch);char *draft=next[q->draft_var].text;unsigned bytes;
      pet_form_utf8(draft,63,&bytes,NULL,NULL);memcpy(draft+bytes,ch,strlen(ch)+1);copy_text(buffer,"");next[f->cursor_var].number=0;
    }else{
      for(unsigned i=0;i<f->count;i++)copy_text(next[f->fields[i].var].text,next[f->fields[i].draft_var].text);
      next[f->active_var].number=0;reset_keyboard(f,next);
    }
  }
  refresh(f,next);memcpy(v,next,sizeof(next));return true;
}
bool pet_form3_text(const pet_form3_t *f,const pet_form3_value_t v[PET_FORM3_VARS],int field,char out[121]){
  if(!out||!state_valid(f,v)||field<0||field>=f->count)return false;
  const pet_form3_field_t *q=&f->fields[field];unsigned index=v[f->active_var].number==1?q->draft_var:q->var;
  copy_text(out,v[index].text);return true;
}
pet_form_replace_result_t pet_form3_replace(const pet_form3_t *f,pet_form3_value_t v[PET_FORM3_VARS],int page,int field,const char *s){
  if(!state_valid(f,v)||page!=f->page||v[f->active_var].number!=1||field<0||field>=f->count||field!=v[f->focus_var].number)return PET_FORM_REPLACE_DENIED;
  unsigned bytes;if(!pet_form_utf8(s,63,&bytes,NULL,NULL))return PET_FORM_REPLACE_INVALID;
  if(bytes>f->fields[field].max_bytes)return PET_FORM_REPLACE_LIMIT;
  if(!strcmp(s,v[f->fields[field].draft_var].text))return PET_FORM_REPLACE_UNCHANGED;
  pet_form3_value_t next[PET_FORM3_VARS];memcpy(next,v,sizeof(next));copy_text(next[f->fields[field].draft_var].text,s);
  copy_text(next[f->buffer_var].text,"");next[f->cursor_var].number=0;refresh(f,next);memcpy(v,next,sizeof(next));return PET_FORM_REPLACE_OK;
}
