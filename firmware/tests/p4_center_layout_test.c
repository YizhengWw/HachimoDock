#include <assert.h>
#include "pet_p4_center_layout.h"
int main(void) {
  for(size_t count=0;count<=32;count++)for(size_t selected=0;selected<(count?count:1);selected++) {
    size_t start=selected/4*4;
    for(int slot=0;slot<4;slot++) {
      int x=24+(slot%2)*304,y=86+(slot/2)*160;
      int expected=start+slot<count?(int)start+slot:-1;
      assert(pet_center_hit(x+10,y+10,selected,count)==expected);
      assert(pet_center_hit(x+287,y+145,selected,count)==expected);
      assert(pet_center_hit(x+288,y+146,selected,count)==-1);
    }
    assert(pet_center_hit(320,420,selected,count)==-1);
    size_t next=pet_center_page(selected,count,1),prev=pet_center_page(selected,count,-1);
    assert(!count||(next<count&&prev<count));
    assert(pet_center_page(next,count,-1)==start);
  }
  assert(pet_center_page(24,25,1)==0);
  assert(pet_center_page(0,25,-1)==24);
}
