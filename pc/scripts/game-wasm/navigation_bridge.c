/* Fixed product navigation, not package-supplied executable code. One bounded
 * input snapshot per synchronous call, no retained JS object identity. */
#include "pet_p4_navigation_core.h"
static pet_nav_history_t history;
static pet_nav_rule_t rule;
static int8_t page;
static unsigned page_count,expected_depth,history_mask;
static bool valid,rule_ready;
void nv_begin(unsigned count,unsigned depth,int current){
  valid=false;rule_ready=false;history=(pet_nav_history_t){0};history_mask=0;
  if(count<1||count>PET_NAV_MAX_PAGES||depth>PET_NAV_MAX_DEPTH||current<0||(unsigned)current>=count)return;
  page_count=count;expected_depth=depth;page=(int8_t)current;history.depth=(uint8_t)depth;valid=true;
}
int nv_history(unsigned index,int value){
  if(!valid||index>=expected_depth||value<0||(unsigned)value>=page_count){valid=false;return 0;}
  history.pages[index]=(uint8_t)value;history_mask|=1U<<index;return 1;
}
int nv_rule(int operation,int target,int from){
  rule_ready=false;
  if(!valid||operation<PET_NAV_NONE||operation>PET_NAV_BACK||from< -1||(from>=0&&(unsigned)from>=page_count)
      ||((operation==PET_NAV_PUSH||operation==PET_NAV_REPLACE||operation==PET_NAV_RESET)
        ?target<0||(unsigned)target>=page_count:target!= -1)){valid=false;return 0;}
  rule=(pet_nav_rule_t){.op=(uint8_t)operation,.target=(int8_t)target,.from_page=(int8_t)from};rule_ready=true;return 1;
}
int nv_allowed(void){
  return valid&&rule_ready&&history_mask==((1U<<expected_depth)-1U)&&pet_nav_allowed(&history,page,&rule);
}
int nv_apply(void){return nv_allowed()&&pet_nav_apply(&history,&page,&rule);}
int nv_page(void){return page;}
unsigned nv_depth(void){return history.depth;}
int nv_at(unsigned index){return index<history.depth?history.pages[index]:-1;}
