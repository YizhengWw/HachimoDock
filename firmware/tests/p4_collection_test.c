#include "pet_p4_collection.h"
#ifdef PET_COLLECTION_WASM
#define check(x) do {if(!(x))return __LINE__;}while(0)
#else
#include <assert.h>
#include <stdio.h>
#define check(x) assert(x)
#endif
#include <string.h>
static pet_collection_state_t state,before;
static pet_collection_view_t view,prior;
static pet_collection_workspace_t workspace;
static pet_collection_row_t rows[PET_COLLECTION_ROWS];
static char keys[PET_COLLECTION_ROWS][12],labels[PET_COLLECTION_ROWS][12];
static char long_text[513];
static uint32_t next_random(uint32_t *seed){*seed=*seed*1664525U+1013904223U;return *seed;}
static pet_collection_result_t project(unsigned count,unsigned visible,pet_collection_operation_t op){
  return pet_collection_project(rows,count,visible,op,&state,&view,&workspace);
}
int pet_collection_selftest(void){
  for(unsigned i=0;i<PET_COLLECTION_ROWS;i++){
    keys[i][0]='k';keys[i][1]=(char)('0'+i/100);keys[i][2]=(char)('0'+i/10%10);keys[i][3]=(char)('0'+i%10);
    labels[i][0]='L';memcpy(labels[i]+1,keys[i]+1,3);
    rows[i]=(pet_collection_row_t){keys[i],labels[i],""};
  }
  state=(pet_collection_state_t){.offset=127};strcpy(state.selected,"k127");before=state;
  check(project(3,2,PET_COLLECTION_STATUS)==PET_COLLECTION_OK&&view.offset==2&&view.selected_index== -1&&!strcmp(view.selection_key,"k127"));
  check(!memcmp(&before,&state,sizeof(state))&&view.rows[0].index==2&&!view.rows[1].present);
  check(project(128,2,PET_COLLECTION_STATUS)==PET_COLLECTION_OK&&view.selected_index==127&&state.offset==127);
  check(project(3,2,PET_COLLECTION_PREVIOUS)==PET_COLLECTION_OK&&state.offset==0&&!strcmp(state.selected,"k127"));
  check(project(3,2,PET_COLLECTION_ROW_1)==PET_COLLECTION_OK&&!strcmp(state.selected,"k001"));
  rows[0]=(pet_collection_row_t){keys[1],labels[1],""};rows[1]=(pet_collection_row_t){keys[0],labels[0],""};
  check(project(3,2,PET_COLLECTION_STATUS)==PET_COLLECTION_OK&&view.selected_index==0&&view.rows[0].selected);
  check(project(3,2,PET_COLLECTION_NEXT)==PET_COLLECTION_OK&&state.offset==2);
  before=state;prior=view;
  check(project(3,2,PET_COLLECTION_ROW_1)==PET_COLLECTION_NO_ROW&&!memcmp(&before,&state,sizeof(state))&&!memcmp(&prior,&view,sizeof(view)));
  check(project(0,2,PET_COLLECTION_STATUS)==PET_COLLECTION_OK&&view.pages==0&&view.selected_index== -1&&!view.can_up&&!view.can_down);
  before=state;prior=view;
  check(project(0,2,PET_COLLECTION_DOWN)==PET_COLLECTION_NO_ROW&&!memcmp(&before,&state,sizeof(state))&&!memcmp(&prior,&view,sizeof(view)));
  rows[0]=(pet_collection_row_t){keys[0],labels[0],""};rows[1]=rows[0];
  check(project(2,2,PET_COLLECTION_STATUS)==PET_COLLECTION_BAD_DATA&&!memcmp(&prior,&view,sizeof(view))&&!memcmp(&before,&state,sizeof(state)));
  rows[1]=(pet_collection_row_t){keys[1],labels[1],""};
  /* UTF-8 scalar truncation, source retained; astral glyph crosses byte 63. */
  for(unsigned i=0;i<128;i++)memcpy(long_text+4*i,"\xf0\x9f\x90\xb1",4);
  long_text[512]=0;rows[0].label=long_text;
  check(project(2,2,PET_COLLECTION_STATUS)==PET_COLLECTION_OK&&view.rows[0].truncated&&strlen(view.rows[0].label)==60&&strlen(long_text)==512);
  const char *bad[]={"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe7\x8c","\xc2\x85","x\n","\x80"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
    rows[0].label=bad[i];before=state;prior=view;
    check(project(2,2,PET_COLLECTION_CLEAR)==PET_COLLECTION_BAD_DATA&&!memcmp(&before,&state,sizeof(state))&&!memcmp(&prior,&view,sizeof(view)));
  }
  rows[0].label=labels[0];memset(state.selected,'x',sizeof(state.selected));prior=view;
  check(project(2,2,PET_COLLECTION_STATUS)==PET_COLLECTION_BAD_STATE&&!memcmp(&prior,&view,sizeof(view)));
  state=(pet_collection_state_t){.offset=128};before=state;
  check(project(2,2,PET_COLLECTION_CLEAR)==PET_COLLECTION_BAD_STATE&&!memcmp(&before,&state,sizeof(state)));
  state=(pet_collection_state_t){0};prior=view;
  check(project(129,2,PET_COLLECTION_STATUS)==PET_COLLECTION_INVALID&&!memcmp(&prior,&view,sizeof(view)));
  check(project(2,0,PET_COLLECTION_STATUS)==PET_COLLECTION_INVALID&&project(2,9,PET_COLLECTION_STATUS)==PET_COLLECTION_INVALID);
  check(project(2,2,(pet_collection_operation_t)256)==PET_COLLECTION_INVALID);
  check(pet_collection_project(rows,2,2,PET_COLLECTION_STATUS,&state,(pet_collection_view_t*)&workspace,&workspace)==PET_COLLECTION_INVALID);
  check(pet_collection_project(rows,2,2,PET_COLLECTION_STATUS,&state,&view,(pet_collection_workspace_t*)&view)==PET_COLLECTION_INVALID);
  strcpy(state.selected,"k000");rows[0].key=state.selected;before=state;
  check(project(2,2,PET_COLLECTION_STATUS)==PET_COLLECTION_INVALID&&!memcmp(&before,&state,sizeof(state)));
  state=(pet_collection_state_t){0};uint32_t seed=17;int selected_key= -1;unsigned saved_offset=0;
  for(unsigned step=0;step<30000;step++){
    unsigned rotation=(next_random(&seed)>>16)%128,count=(next_random(&seed)>>16)%129,visible=(next_random(&seed)>>16)%8+1;
    pet_collection_operation_t op=(pet_collection_operation_t)((next_random(&seed)>>16)%14);
    for(unsigned i=0;i<128;i++){unsigned key=(i+rotation)%128;rows[i]=(pet_collection_row_t){keys[key],labels[key],""};}
    unsigned last=(count?count-1:0)/visible*visible,offset=saved_offset<last?saved_offset:last;offset=offset/visible*visible;
    int selected_index= -1;for(unsigned i=0;i<count;i++)if((int)((i+rotation)%128)==selected_key)selected_index=(int)i;
    bool rejected=false;
    if(op==PET_COLLECTION_NEXT)offset=offset+visible<last?offset+visible:last;
    else if(op==PET_COLLECTION_PREVIOUS)offset=offset>visible?offset-visible:0;
    else if(op==PET_COLLECTION_CLEAR)selected_key= -1;
    else if(op==PET_COLLECTION_UP||op==PET_COLLECTION_DOWN){
      if(!count)rejected=true;
      else {selected_index=selected_index<0?0:op==PET_COLLECTION_UP?(selected_index?selected_index-1:0):((unsigned)selected_index+1<count?selected_index+1:(int)count-1);
        selected_key=(selected_index+(int)rotation)%128;offset=(unsigned)selected_index/visible*visible;}
    }else if(op>=PET_COLLECTION_ROW_0){
      unsigned slot=(unsigned)op-PET_COLLECTION_ROW_0;
      if(slot>=visible||offset+slot>=count)rejected=true;
      else selected_key=(int)((offset+slot+rotation)%128);
    }
    before=state;prior=view;pet_collection_result_t result=project(count,visible,op);
    if(rejected){check(result==PET_COLLECTION_NO_ROW&&!memcmp(&before,&state,sizeof(state))&&!memcmp(&prior,&view,sizeof(view)));continue;}
    check(result==PET_COLLECTION_OK);
    if(op!=PET_COLLECTION_STATUS)saved_offset=offset;
    check(state.offset==saved_offset&&(!state.selected[0]?selected_key== -1:!strcmp(state.selected,keys[selected_key])));
    check(view.count==count&&view.offset==offset&&view.visible==visible&&view.page==offset/visible&&view.pages==(count?last/visible+1:0));
    selected_index= -1;for(unsigned i=0;i<count;i++)if((int)((i+rotation)%128)==selected_key)selected_index=(int)i;
    check(view.selected_index==selected_index);
    check(view.can_previous==(offset>0)&&view.can_next==(offset<last));
    check(view.can_up==(count&&(selected_index<0||selected_index>0)));
    check(view.can_down==(count&&(selected_index<0||(unsigned)selected_index+1<count)));
    for(unsigned slot=0;slot<8;slot++){
      bool present=slot<visible&&offset+slot<count;
      check(view.rows[slot].present==present&&view.rows[slot].index==(present?(int)(offset+slot):-1));
      if(present){check(!strcmp(view.rows[slot].key,keys[(offset+slot+rotation)%128]));check(view.rows[slot].selected==(selected_index==(int)(offset+slot)));}
      else check(!view.rows[slot].key[0]&&!view.rows[slot].label[0]&&!view.rows[slot].detail[0]&&!view.rows[slot].selected&&!view.rows[slot].truncated);
    }
  }
  return 0;
}
#ifndef PET_COLLECTION_WASM
int main(void){int result=pet_collection_selftest();printf("{\"result\":%d,\"trials\":30000,\"viewBytes\":%zu,\"workspaceBytes\":%zu}\n",result,sizeof(view),sizeof(workspace));return result;}
#endif
