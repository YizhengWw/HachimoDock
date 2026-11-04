/* Fixed product preview only. No package code, imports, IO or saved data.
 * The JS adapter creates a new instance before replacing immutable source rows;
 * invalid replacement must not overwrite the old instance/model. */
#include "pet_p4_collection.h"
#include <string.h>
typedef struct {char key[64],label[513],detail[513];} collection_source_t;
static collection_source_t source[PET_COLLECTION_ROWS];
static char seed_key[64];
static pet_collection_row_t rows[PET_COLLECTION_ROWS];
static pet_collection_state_t state;
static pet_collection_view_t view;
static pet_collection_workspace_t workspace;
static unsigned count,visible;
static bool attempted,loaded;
void *cv_buffer(void){return source;}
void *cv_seed_key(void){return seed_key;}
unsigned cv_stride(void){return sizeof(collection_source_t);}
int cv_load(unsigned size,unsigned page_size,unsigned offset){
  if(attempted)return PET_COLLECTION_INVALID;
  attempted=true;
  if(size>PET_COLLECTION_ROWS||page_size<1||page_size>PET_COLLECTION_VISIBLE||offset>=PET_COLLECTION_ROWS)return PET_COLLECTION_INVALID;
  state.offset=(uint16_t)offset;memcpy(state.selected,seed_key,sizeof(seed_key));
  for(unsigned i=0;i<size;i++){
    /* Even a raw caller must supply separately terminated fields. No read may
     * scan into the next field merely because it contains a zero byte. */
    if(source[i].key[63]||source[i].label[512]||source[i].detail[512])return PET_COLLECTION_BAD_DATA;
    rows[i]=(pet_collection_row_t){source[i].key,source[i].label,source[i].detail};
  }
  count=size;visible=page_size;
  pet_collection_result_t result=pet_collection_project(rows,count,visible,PET_COLLECTION_STATUS,&state,&view,&workspace);
  loaded=result==PET_COLLECTION_OK;return result;
}
int cv_apply(unsigned operation){
  if(!loaded||operation>PET_COLLECTION_ROW_7)return PET_COLLECTION_INVALID;
  return pet_collection_project(rows,count,visible,(pet_collection_operation_t)operation,&state,&view,&workspace);
}
unsigned cv_state_offset(void){return state.offset;}
const char *cv_state_key(void){return state.selected;}
const char *cv_text(unsigned field){return field?view.selected_label:view.selection_key;}
unsigned cv_metric(unsigned field){
  switch(field){
    case 0:return view.count;case 1:return view.offset;case 2:return view.page;case 3:return view.pages;
    case 4:return view.visible;case 5:return view.can_previous;case 6:return view.can_next;
    case 7:return view.can_up;case 8:return view.can_down;default:return 0;
  }
}
int cv_selected_index(void){return view.selected_index;}
const char *cv_row_text(unsigned index,unsigned field){
  if(index>=PET_COLLECTION_VISIBLE)return "";
  return field==0?view.rows[index].key:field==1?view.rows[index].label:field==2?view.rows[index].detail:"";
}
int cv_row_metric(unsigned index,unsigned field){
  if(index>=PET_COLLECTION_VISIBLE)return -1;
  const pet_collection_visible_t *row=&view.rows[index];
  switch(field){case 0:return row->present;case 1:return row->index;case 2:return row->selected;case 3:return row->truncated;default:return -1;}
}
