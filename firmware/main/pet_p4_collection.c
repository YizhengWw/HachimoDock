#include "pet_p4_collection.h"
#include "pet_p4_collection_internal.h"
#include <string.h>
static bool overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an&&bn&&(x<=y?y-x<an:x-y<bn);
}
/* Reject invalid scalars/encodings before treating bytes as text. ASCII, C1
 * controls and source byte budgets match the existing PC collection contract. */
static bool text_length(const char *s,size_t limit,size_t *length){
  if(!s)return false;
  size_t i=0;
  for(;;){
    unsigned c=(unsigned char)s[i];if(!c){*length=i;return true;}
    if(i==limit)return false;
    unsigned n=1;uint32_t scalar=c;
    if(c>=0xc2&&c<=0xdf){n=2;scalar=c&31U;}
    else if(c>=0xe0&&c<=0xef){n=3;scalar=c&15U;}
    else if(c>=0xf0&&c<=0xf4){n=4;scalar=c&7U;}
    else if(c>=0x80)return false;
    if(n>limit-i)return false;
    for(unsigned j=1;j<n;j++){
      unsigned continuation=(unsigned char)s[i+j];if((continuation&0xc0U)!=0x80U)return false;
      scalar=(scalar<<6)|(continuation&63U);
    }
    if((n==2&&scalar<0x80)||(n==3&&scalar<0x800)||(n==4&&scalar<0x10000)
      ||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff)||scalar<32||(scalar>=127&&scalar<=159))return false;
    i+=n;
  }
}
static void short_text(char out[PET_COLLECTION_TEXT],const char *source){
  size_t end=0;while(end<PET_COLLECTION_TEXT-1&&source[end])end++;
  if(end==PET_COLLECTION_TEXT-1&&source[end]){
    /* Byte 63 can be a continuation of a scalar beginning earlier. */
    while(end&&((unsigned char)source[end]&0xc0U)==0x80U)end--;
  }
  if(end)memcpy(out,source,end);
  out[end]=0;
}
bool pet_collection_text_valid(const char *text,size_t maximum_bytes,bool nonempty){
  size_t length;
  return maximum_bytes<=PET_COLLECTION_SOURCE_TEXT&&text_length(text,maximum_bytes,&length)&&(!nonempty||length!=0);
}
static bool source_alias(const char *source,size_t bytes,const pet_collection_state_t *state,
    const pet_collection_view_t *out,const pet_collection_workspace_t *work){
  return overlap(source,bytes,state,sizeof(*state))||overlap(source,bytes,out,sizeof(*out))
    ||overlap(source,bytes,work,sizeof(*work));
}
static pet_collection_result_t project(const pet_collection_row_t *rows,unsigned count,
    unsigned visible,pet_collection_operation_t op,pet_collection_state_t *state,
    pet_collection_view_t *out,pet_collection_workspace_t *work,bool validate_source){
  if(!state||!out||!work||count>PET_COLLECTION_ROWS||!visible||visible>PET_COLLECTION_VISIBLE
      ||op<PET_COLLECTION_STATUS||op>PET_COLLECTION_ROW_7||(!rows&&count))return PET_COLLECTION_INVALID;
  size_t bytes=(size_t)count*sizeof(*rows);
  if(overlap(state,sizeof(*state),out,sizeof(*out))||overlap(state,sizeof(*state),work,sizeof(*work))
      ||overlap(out,sizeof(*out),work,sizeof(*work))||overlap(rows,bytes,state,sizeof(*state))
      ||overlap(rows,bytes,out,sizeof(*out))||overlap(rows,bytes,work,sizeof(*work)))return PET_COLLECTION_INVALID;
  size_t selected_bytes;
  if(state->offset>=PET_COLLECTION_ROWS||!text_length(state->selected,PET_COLLECTION_TEXT-1,&selected_bytes))return PET_COLLECTION_BAD_STATE;
  int selected=-1;
  for(unsigned i=0;i<count;i++){
    if(validate_source){
      size_t key,label,detail;
      if(!text_length(rows[i].key,PET_COLLECTION_TEXT-1,&key)||!key
         ||!text_length(rows[i].label,PET_COLLECTION_SOURCE_TEXT,&label)
         ||!text_length(rows[i].detail,PET_COLLECTION_SOURCE_TEXT,&detail))return PET_COLLECTION_BAD_DATA;
      if(source_alias(rows[i].key,key+1,state,out,work)||source_alias(rows[i].label,label+1,state,out,work)
         ||source_alias(rows[i].detail,detail+1,state,out,work))return PET_COLLECTION_INVALID;
      for(unsigned j=0;j<i;j++)if(!strcmp(rows[i].key,rows[j].key))return PET_COLLECTION_BAD_DATA;
    }
    if(selected_bytes&&!strcmp(rows[i].key,state->selected))selected=(int)i;
  }
  memset(work,0,sizeof(*work));work->state=*state;
  unsigned last=(count?count-1:0)/visible*visible;
  unsigned offset=state->offset<last?state->offset:last;offset=offset/visible*visible;
  if(op==PET_COLLECTION_NEXT)offset=offset+visible<last?offset+visible:last;
  else if(op==PET_COLLECTION_PREVIOUS)offset=offset>visible?offset-visible:0;
  else if(op==PET_COLLECTION_CLEAR)work->state.selected[0]=0;
  else if(op==PET_COLLECTION_UP||op==PET_COLLECTION_DOWN){
    if(!count)return PET_COLLECTION_NO_ROW;
    selected=selected<0?0:op==PET_COLLECTION_UP?(selected?selected-1:0):((unsigned)selected+1<count?selected+1:(int)count-1);
    short_text(work->state.selected,rows[selected].key);offset=(unsigned)selected/visible*visible;
  }else if(op>=PET_COLLECTION_ROW_0){
    unsigned slot=(unsigned)op-PET_COLLECTION_ROW_0;
    if(slot>=visible||offset+slot>=count)return PET_COLLECTION_NO_ROW;
    short_text(work->state.selected,rows[offset+slot].key);
  }
  if(op!=PET_COLLECTION_STATUS)work->state.offset=(uint16_t)offset;
  pet_collection_view_t *v=&work->view;
  v->visible=(uint8_t)visible;v->count=(uint16_t)count;v->offset=(uint16_t)offset;
  v->page=(uint16_t)(offset/visible);v->pages=(uint16_t)(count?last/visible+1:0);v->selected_index=-1;
  short_text(v->selection_key,work->state.selected);
  for(unsigned i=0;i<count;i++)if(work->state.selected[0]&&!strcmp(rows[i].key,work->state.selected)){
    v->selected_index=(int16_t)i;short_text(v->selected_label,rows[i].label);break;
  }
  for(unsigned slot=0;slot<PET_COLLECTION_VISIBLE;slot++){
    pet_collection_visible_t *r=&v->rows[slot];r->index=-1;
    unsigned i=offset+slot;if(slot>=visible||i>=count)continue;
    r->present=true;r->selected=v->selected_index==(int)i;r->index=(int16_t)i;
    short_text(r->key,rows[i].key);short_text(r->label,rows[i].label);short_text(r->detail,rows[i].detail);
    r->truncated=strlen(rows[i].label)>=PET_COLLECTION_TEXT||strlen(rows[i].detail)>=PET_COLLECTION_TEXT;
  }
  v->can_previous=offset>0;v->can_next=offset<last;
  v->can_up=count&&(v->selected_index<0||v->selected_index>0);
  v->can_down=count&&(v->selected_index<0||(unsigned)v->selected_index+1<count);
  *out=*v;if(op!=PET_COLLECTION_STATUS)*state=work->state;
  return PET_COLLECTION_OK;
}
pet_collection_result_t pet_collection_project(const pet_collection_row_t *rows,unsigned count,
    unsigned visible,pet_collection_operation_t op,pet_collection_state_t *state,
    pet_collection_view_t *out,pet_collection_workspace_t *work){
  return project(rows,count,visible,op,state,out,work,true);
}
pet_collection_result_t pet_collection_project_validated(const pet_collection_row_t *rows,unsigned count,
    unsigned visible,pet_collection_operation_t op,pet_collection_state_t *state,
    pet_collection_view_t *out,pet_collection_workspace_t *work){
  return project(rows,count,visible,op,state,out,work,false);
}
