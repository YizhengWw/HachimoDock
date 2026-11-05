/* Actual native owned-model bridge and shared core; synthetic UI authority.
 * No device, installed control, source service, user record or physical grant. */
#include "pet_p4_collection_host.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
static pet_collection_host_t hosts[3],prior_host;
static pet_collection_dataset_t banks[3],prior_bank;
static pet_collection_host_workspace_t work,reentry_work;
static pet_collection_view_t view,prior_view,oracle_view;
static pet_collection_workspace_t oracle_work;
static pet_collection_state_t seed,oracle_state,saved;
static pet_collection_observation_t observed,old_observed;
static pet_collection_row_t rows[128];
static char keys[128][64],labels[128][513];
static uint8_t identity[32]={1};
typedef struct {unsigned index,checks,reentries,deny_at;bool live,reenter;} context_t;
static context_t contexts[3];
static bool current(void*context,uint64_t generation){
  context_t*c=context;assert(generation==100+c->index);c->checks++;
  if(c->reenter){
    pet_collection_host_t*h=&hosts[c->index];pet_collection_observation_t observation={0};
    pet_collection_state_t state;pet_collection_view_t output;
    pet_collection_authority_t authority=h->authority;
    assert(pet_collection_host_read(h,generation,&output,&observation)==PET_COLLECTION_HOST_BUSY);
    assert(pet_collection_host_observation_current(h,generation,&observed)==PET_COLLECTION_HOST_BUSY);
    assert(pet_collection_host_apply(h,generation,PET_COLLECTION_NEXT,&observed,&reentry_work)==PET_COLLECTION_HOST_BUSY);
    assert(pet_collection_host_publish(h,generation,&banks[2],&seed,2,&reentry_work)==PET_COLLECTION_HOST_BUSY);
    assert(pet_collection_host_snapshot(h,generation,&state)==PET_COLLECTION_HOST_BUSY);
    assert(pet_collection_host_cancel(h,generation)==PET_COLLECTION_HOST_BUSY);
    assert(!pet_collection_host_init(h,&authority));c->reentries++;
  }
  return c->live&&(!c->deny_at||c->checks<c->deny_at);
}
static uint64_t init(unsigned index){
  contexts[index]=(context_t){.index=index,.live=true};
  pet_collection_authority_t authority={.context=&contexts[index],.generation=100+index,.current=current};
  memcpy(authority.identity,identity,sizeof(identity));
  assert(pet_collection_host_init(&hosts[index],&authority));return authority.generation;
}
static void source(unsigned count,unsigned rotation){
  for(unsigned i=0;i<128;i++){
    snprintf(keys[i],sizeof(keys[i]),"key-%03u",i);snprintf(labels[i],sizeof(labels[i]),"label-%03u",i);
  }
  for(unsigned i=0;i<count;i++){unsigned key=(i+rotation)%128;rows[i]=(pet_collection_row_t){keys[key],labels[key],""};}
}
static void prepare(unsigned bank,uint64_t revision,unsigned count,unsigned rotation){
  source(count,rotation);
  assert(pet_collection_dataset_prepare(&banks[bank],identity,revision,rows,count,&work)==PET_COLLECTION_HOST_OK);
}
static uint64_t setup(unsigned count,unsigned visible){
  uint64_t g=init(0);prepare(0,1,count,0);seed=(pet_collection_state_t){0};
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,visible,&work)==PET_COLLECTION_HOST_OK);return g;
}
static pet_collection_observation_t read(unsigned index){
  pet_collection_observation_t result;
  assert(pet_collection_host_read(&hosts[index],100+index,&view,&result)==PET_COLLECTION_HOST_OK);return result;
}
static void test_owned_preparation_and_scrub(void){
  prepare(0,1,128,0);strcpy(keys[0],"changed");strcpy(labels[0],"changed");
  assert(!strcmp(banks[0].rows[0].key,"key-000")&&!strcmp(banks[0].rows[0].label,"label-000"));
  prepare(0,2,1,0);assert(banks[0].count==1&&!banks[0].records[127].key[0]&&!banks[0].rows[127].key);
  prior_bank=banks[0];rows[1]=rows[0];
  assert(pet_collection_dataset_prepare(&banks[0],identity,3,rows,2,&work)==PET_COLLECTION_HOST_BAD_DATA);
  assert(!memcmp(&banks[0],&prior_bank,sizeof(prior_bank)));
  uint64_t g=init(0);assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,2,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_dataset_prepare(&banks[0],identity,3,rows,1,&work)==PET_COLLECTION_HOST_INVALID&&banks[0].bound==&hosts[0]);
}
static void test_seed_required_and_invalid_restore_preserved(void){
  uint64_t g=init(0);prepare(0,1,3,0);prior_host=hosts[0];
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],NULL,2,&work)==PET_COLLECTION_HOST_BAD_STATE);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&!banks[0].bound);
  seed.offset=128;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,2,&work)==PET_COLLECTION_HOST_BAD_STATE&&!banks[0].bound);
  seed=(pet_collection_state_t){.offset=127};strcpy(seed.selected,"hidden-key");
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,2,&work)==PET_COLLECTION_HOST_OK);
  observed=read(0);assert(view.offset==2&&view.selected_index==-1&&!strcmp(view.selection_key,"hidden-key"));
  assert(hosts[0].state.offset==127&&!strcmp(hosts[0].state.selected,"hidden-key")&&observed.epoch==1);
}
static void test_reorder_filter_restore_invalidates_gestures(void){
  uint64_t g=setup(3,2);observed=read(0);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_1,&observed,&work)==PET_COLLECTION_HOST_OK);
  old_observed=read(0);assert(!strcmp(view.selection_key,"key-001"));
  prepare(1,2,3,1);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_OK&&!banks[0].bound);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_0,&old_observed,&work)==PET_COLLECTION_HOST_STALE);
  observed=read(0);assert(view.selected_index==0&&!strcmp(view.selection_key,"key-001"));
  prepare(0,3,0,0);assert(pet_collection_host_publish(&hosts[0],g,&banks[0],NULL,2,&work)==PET_COLLECTION_HOST_OK);
  observed=read(0);assert(!view.count&&view.selected_index==-1&&!strcmp(view.selection_key,"key-001"));
  prepare(1,4,3,0);assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_OK);
  observed=read(0);assert(view.selected_index==1&&!strcmp(view.selection_key,"key-001"));
}
static void test_identity_revision_and_bad_seed_transaction(void){
  uint64_t g=setup(3,2);prepare(1,2,3,0);prior_host=hosts[0];
  banks[1].identity[0]=2;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_STALE);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&banks[0].bound==&hosts[0]&&!banks[1].bound);
  banks[1].identity[0]=1;banks[1].revision=1;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_STALE);
  banks[1].revision=2;seed.offset=128;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],&seed,2,&work)==PET_COLLECTION_HOST_BAD_STATE);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&banks[0].bound==&hosts[0]&&!banks[1].bound);
}
static void test_each_action_consumes_observation_and_away_back(void){
  uint64_t g=setup(3,2);observed=read(0);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_0,&observed,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_0,&observed,&work)==PET_COLLECTION_HOST_STALE);
  old_observed=read(0);uint64_t epoch=old_observed.epoch;
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_0,&old_observed,&work)==PET_COLLECTION_HOST_OK);
  assert(hosts[0].epoch==epoch+1);observed=read(0);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_NEXT,&observed,&work)==PET_COLLECTION_HOST_OK);
  observed=read(0);assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_PREVIOUS,&observed,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_1,&old_observed,&work)==PET_COLLECTION_HOST_STALE);
}
static void test_missing_row_has_no_commit_or_epoch(void){
  uint64_t g=setup(1,2);observed=read(0);prior_host=hosts[0];
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_1,&observed,&work)==PET_COLLECTION_HOST_NO_ROW);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&banks[0].bound==&hosts[0]);
  memset(&observed,0,sizeof(observed));assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_DOWN,&observed,&work)==PET_COLLECTION_HOST_STALE);
}
static void test_cancel_no_resume_and_bank_reuse_new_owner(void){
  uint64_t g=setup(3,2);observed=read(0);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_1,&observed,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_cancel(&hosts[0],g)==PET_COLLECTION_HOST_OK&&!banks[0].bound);
  assert(pet_collection_host_snapshot(&hosts[0],g,&saved)==PET_COLLECTION_HOST_OK&&!strcmp(saved.selected,"key-001"));
  unsigned checks=contexts[0].checks;contexts[0].live=true;
  assert(pet_collection_host_read(&hosts[0],g,&view,&observed)==PET_COLLECTION_HOST_STOPPED&&contexts[0].checks==checks);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],&saved,2,&work)==PET_COLLECTION_HOST_STOPPED);
  uint64_t next=init(1);prepare(0,9,3,1);
  assert(pet_collection_host_publish(&hosts[1],next,&banks[0],&saved,2,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_cancel(&hosts[0],g)==PET_COLLECTION_HOST_OK&&banks[0].bound==&hosts[1]);
  assert(pet_collection_host_cancel(&hosts[1],g)==PET_COLLECTION_HOST_STALE&&banks[0].bound==&hosts[1]);
  observed=read(1);assert(view.selected_index==0&&!strcmp(view.selection_key,"key-001"));
}
static void test_authority_loss_at_publish_and_input_commit(void){
  uint64_t g=setup(3,2);prepare(1,2,3,1);prior_host=hosts[0];contexts[0].deny_at=contexts[0].checks+2;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_DENIED);
  assert(hosts[0].stopped&&!banks[0].bound&&!banks[1].bound&&!memcmp(&hosts[0].state,&prior_host.state,sizeof(saved)));
  assert(!memcmp(&hosts[0].view,&prior_host.view,sizeof(view))&&hosts[0].epoch==prior_host.epoch);
  uint64_t g1=init(1);seed=(pet_collection_state_t){0};prepare(1,3,3,0);
  assert(pet_collection_host_publish(&hosts[1],g1,&banks[1],&seed,2,&work)==PET_COLLECTION_HOST_OK);
  observed=read(1);prior_host=hosts[1];contexts[1].deny_at=contexts[1].checks+2;
  assert(pet_collection_host_apply(&hosts[1],g1,PET_COLLECTION_ROW_1,&observed,&work)==PET_COLLECTION_HOST_DENIED);
  assert(hosts[1].stopped&&!banks[1].bound&&!memcmp(&hosts[1].state,&prior_host.state,sizeof(saved))&&hosts[1].epoch==prior_host.epoch);
}
static void test_read_refusal_preserves_outputs_and_history(void){
  uint64_t g=setup(3,2);memset(&view,0xa5,sizeof(view));prior_view=view;
  observed=(pet_collection_observation_t){.epoch=99};old_observed=observed;contexts[0].deny_at=contexts[0].checks+2;
  assert(pet_collection_host_read(&hosts[0],g,&view,&observed)==PET_COLLECTION_HOST_DENIED);
  assert(!memcmp(&view,&prior_view,sizeof(view))&&!memcmp(&observed,&old_observed,sizeof(observed))&&hosts[0].stopped&&!banks[0].bound);
  assert(pet_collection_host_snapshot(&hosts[0],g,&saved)==PET_COLLECTION_HOST_OK&&!saved.selected[0]);
  contexts[0].deny_at=0;assert(pet_collection_host_read(&hosts[0],g,&view,&observed)==PET_COLLECTION_HOST_STOPPED);
}
static void test_callback_reentry_all_paths(void){
  uint64_t g=setup(3,2);prepare(2,5,3,0);contexts[0].reenter=true;
  observed=read(0);assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_ROW_0,&observed,&work)==PET_COLLECTION_HOST_OK);
  assert(contexts[0].reentries);assert(pet_collection_host_cancel(&hosts[0],g)==PET_COLLECTION_HOST_OK);
}
static void test_aliases_before_callbacks_or_writes(void){
  uint64_t g=setup(3,2);prepare(1,2,3,0);prior_host=hosts[0];unsigned checks=contexts[0].checks;
  assert(pet_collection_host_read(&hosts[0],g,&hosts[0].view,&observed)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_read(&hosts[0],g,&view,(void*)&view)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_read(&hosts[0],g,(void*)&banks[0].records[0],&observed)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_NEXT,(void*)&hosts[0],&work)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_NEXT,&observed,(void*)&banks[0])==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],&hosts[0].state,2,&work)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],NULL,2,&work)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_snapshot(&hosts[0],g,&hosts[0].state)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_host_read(&hosts[0],g+1,&view,&observed)==PET_COLLECTION_HOST_STALE);
  assert(contexts[0].checks==checks&&!memcmp(&hosts[0],&prior_host,sizeof(prior_host)));
}
static void test_preparation_bad_text_arguments_and_old_bank_preserved(void){
  prepare(0,1,2,0);prior_bank=banks[0];
  const char*bad[]={"\xc0\x80","\xed\xa0\x80","\xc2\x85","bad\n"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
    rows[0].label=bad[i];assert(pet_collection_dataset_prepare(&banks[0],identity,2,rows,2,&work)==PET_COLLECTION_HOST_BAD_DATA);
    assert(!memcmp(&banks[0],&prior_bank,sizeof(prior_bank)));
  }
  source(2,0);rows[0].key=banks[0].records[0].key;
  assert(pet_collection_dataset_prepare(&banks[0],identity,2,rows,2,&work)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_dataset_prepare(&banks[0],identity,2,banks[0].rows,2,&work)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_dataset_prepare(&banks[0],identity,2,rows,129,&work)==PET_COLLECTION_HOST_INVALID);
  assert(pet_collection_dataset_prepare(&banks[0],identity,0,rows,2,&work)==PET_COLLECTION_HOST_INVALID);
  assert(!memcmp(&banks[0],&prior_bank,sizeof(prior_bank)));
}
static void test_bank_binding_loss_latches_without_releasing_foreign_owner(void){
  uint64_t g=setup(3,2);banks[0].bound=&hosts[1];
  assert(pet_collection_host_read(&hosts[0],g,&view,&observed)==PET_COLLECTION_HOST_BAD_STATE&&hosts[0].stopped);
  assert(banks[0].bound==&hosts[1]);banks[0].bound=&hosts[0];
  assert(pet_collection_host_read(&hosts[0],g,&view,&observed)==PET_COLLECTION_HOST_STOPPED);
  assert(pet_collection_host_cancel(&hosts[0],g)==PET_COLLECTION_HOST_OK&&banks[0].bound==&hosts[0]);
}
static void test_cached_paint_never_reads_dataset_text_or_rows(void){
  uint64_t g=setup(128,8);(void)g;
  __asan_poison_memory_region(banks[0].records,sizeof(banks[0].records));
  __asan_poison_memory_region(banks[0].rows,sizeof(banks[0].rows));
  for(unsigned i=0;i<10000;i++){observed=read(0);assert(view.count==128&&!strcmp(view.rows[0].key,"key-000")&&observed.epoch==1);view.rows[0].key[0]='X';}
  __asan_unpoison_memory_region(banks[0].records,sizeof(banks[0].records));
  __asan_unpoison_memory_region(banks[0].rows,sizeof(banks[0].rows));
  observed=read(0);assert(!strcmp(view.rows[0].key,"key-000")&&!strcmp(banks[0].records[0].key,"key-000"));
}
static void test_maximum_text_is_owned_and_display_only_truncated(void){
  uint64_t g=init(0);source(128,0);
  memset(keys[0],'k',63);keys[0][63]=0;
  for(unsigned i=0;i<128;i++)memcpy(labels[0]+i*4,"\xf0\x9f\x90\xbe",4);
  labels[0][512]=0;
  assert(pet_collection_dataset_prepare(&banks[0],identity,1,rows,128,&work)==PET_COLLECTION_HOST_OK);
  strcpy(seed.selected,keys[0]);assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,8,&work)==PET_COLLECTION_HOST_OK);
  observed=read(0);assert(view.rows[0].truncated&&strlen(view.rows[0].label)==60&&strlen(view.rows[0].key)==63);
  assert(strlen(banks[0].records[0].label)==512&&!strcmp(banks[0].records[0].label,labels[0]));
}
static void test_epoch_and_revision_exhaustion_never_reset(void){
  uint64_t g=setup(3,2);prepare(1,UINT64_MAX,3,1);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_OK);
  prepare(0,UINT64_MAX,3,0);assert(pet_collection_host_publish(&hosts[0],g,&banks[0],NULL,2,&work)==PET_COLLECTION_HOST_STALE);
  hosts[0].epoch=UINT64_MAX;observed=read(0);prior_host=hosts[0];
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_DOWN,&observed,&work)==PET_COLLECTION_HOST_EXHAUSTED);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&banks[1].bound==&hosts[0]);
}
static uint32_t random_next(uint32_t*s){*s=*s*1664525U+1013904223U;return *s;}
static void test_shared_core_oracle_mixed_owned_turns(void){
  uint64_t g=setup(128,8),revision=1;unsigned active=0,count=128,visible=8,accepted=0,replaced=0;
  uint32_t random=17;oracle_state=(pet_collection_state_t){0};
  for(unsigned turn=0;turn<10000;turn++){
    if(turn%128==0){
      unsigned next=1-active;count=random_next(&random)%129;visible=1+random_next(&random)%8;
      prepare(next,++revision,count,random_next(&random)%128);
      assert(pet_collection_host_publish(&hosts[0],g,&banks[next],NULL,visible,&work)==PET_COLLECTION_HOST_OK);active=next;replaced++;
    }
    observed=read(0);pet_collection_operation_t op=(pet_collection_operation_t)(random_next(&random)%14);
    pet_collection_result_t expected=pet_collection_project(banks[active].rows,count,visible,op,&oracle_state,&oracle_view,&oracle_work);
    if(op==PET_COLLECTION_STATUS)assert(expected==PET_COLLECTION_OK);
    else{
      uint64_t before=hosts[0].epoch;pet_collection_host_result_t actual=pet_collection_host_apply(&hosts[0],g,op,&observed,&work);
      if(expected==PET_COLLECTION_NO_ROW)assert(actual==PET_COLLECTION_HOST_NO_ROW&&hosts[0].epoch==before);
      else{assert(expected==PET_COLLECTION_OK&&actual==PET_COLLECTION_HOST_OK&&hosts[0].epoch==before+1);accepted++;}
    }
    if(expected==PET_COLLECTION_OK)assert(!memcmp(&hosts[0].view,&oracle_view,sizeof(view)));
    assert(!memcmp(&hosts[0].state,&oracle_state,sizeof(saved)));
  }
  assert(accepted>1000&&replaced>70);assert(pet_collection_host_cancel(&hosts[0],g)==PET_COLLECTION_HOST_OK);
}
static void test_capture_rasterize_publish_context_recheck(void){
  uint64_t g=setup(3,2);observed=read(0);prior_view=view;
  assert(pet_collection_host_observation_current(&hosts[0],g,&observed)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_NEXT,&observed,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_observation_current(&hosts[0],g,&observed)==PET_COLLECTION_HOST_STALE);
  assert(!memcmp(&view,&prior_view,sizeof(view))); /* captured pixels/data stay immutable */
  observed=read(0);prepare(1,2,3,1);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_observation_current(&hosts[0],g,&observed)==PET_COLLECTION_HOST_STALE);
  observed=read(0);assert(pet_collection_host_cancel(&hosts[0],g)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_observation_current(&hosts[0],g,&observed)==PET_COLLECTION_HOST_STOPPED);
}
static void test_receiver_identity_is_fixed_before_first_publication(void){
  contexts[0]=(context_t){.index=0,.live=true};
  pet_collection_authority_t authority={.context=&contexts[0],.generation=100,.current=current};
  assert(!pet_collection_host_init(&hosts[0],&authority)&&!hosts[0].marker);
  memcpy(authority.identity,identity,sizeof(identity));assert(pet_collection_host_init(&hosts[0],&authority));
  authority.identity[0]=2; /* Caller mutation cannot retarget an initialized UI. */
  source(3,0);assert(pet_collection_dataset_prepare(&banks[0],authority.identity,1,rows,3,&work)==PET_COLLECTION_HOST_OK);
  prior_host=hosts[0];
  assert(pet_collection_host_publish(&hosts[0],100,&banks[0],&seed,2,&work)==PET_COLLECTION_HOST_STALE);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&!banks[0].bound&&!hosts[0].published);
  prepare(1,1,3,0);assert(pet_collection_host_publish(&hosts[0],100,&banks[1],&seed,2,&work)==PET_COLLECTION_HOST_OK);
  assert(hosts[0].identity[0]==1&&banks[1].bound==&hosts[0]);
}
static void test_foreign_live_bank_and_observation_cannot_cross_owners(void){
  uint64_t g=setup(3,2);uint64_t next=init(1);prepare(1,2,3,1);
  assert(pet_collection_host_publish(&hosts[1],next,&banks[1],&seed,2,&work)==PET_COLLECTION_HOST_OK);
  prior_host=hosts[0];
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_BAD_DATA);
  assert(!memcmp(&hosts[0],&prior_host,sizeof(prior_host))&&banks[0].bound==&hosts[0]&&banks[1].bound==&hosts[1]);
  source(3,0);assert(pet_collection_dataset_prepare(&banks[1],identity,3,rows,3,&work)==PET_COLLECTION_HOST_INVALID);
  observed=read(1);observed.generation=g;observed.epoch=hosts[0].epoch;observed.revision=hosts[0].revision;
  assert(pet_collection_host_apply(&hosts[0],g,PET_COLLECTION_DOWN,&observed,&work)==PET_COLLECTION_HOST_STALE);
  assert(pet_collection_host_cancel(&hosts[1],next)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_publish(&hosts[0],g,&banks[1],NULL,2,&work)==PET_COLLECTION_HOST_OK);
  assert(pet_collection_host_cancel(&hosts[1],next)==PET_COLLECTION_HOST_OK&&banks[1].bound==&hosts[0]);
}
static void test_initial_authority_refusal_never_publishes_defaults(void){
  uint64_t g=init(0);prepare(0,1,3,0);contexts[0].deny_at=1;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,2,&work)==PET_COLLECTION_HOST_DENIED);
  assert(hosts[0].stopped&&!hosts[0].published&&!hosts[0].epoch&&!banks[0].bound);
  contexts[0].deny_at=0;
  assert(pet_collection_host_publish(&hosts[0],g,&banks[0],&seed,2,&work)==PET_COLLECTION_HOST_STOPPED);
  assert(pet_collection_host_snapshot(&hosts[0],g,&saved)==PET_COLLECTION_HOST_UNPUBLISHED);
}
int main(int argc,char**argv){
  assert(argc==2);unsigned c=(unsigned)strtoul(argv[1],NULL,10);
  void(*cases[])(void)={test_owned_preparation_and_scrub,test_seed_required_and_invalid_restore_preserved,
    test_reorder_filter_restore_invalidates_gestures,test_identity_revision_and_bad_seed_transaction,
    test_each_action_consumes_observation_and_away_back,test_missing_row_has_no_commit_or_epoch,
    test_cancel_no_resume_and_bank_reuse_new_owner,test_authority_loss_at_publish_and_input_commit,
    test_read_refusal_preserves_outputs_and_history,test_callback_reentry_all_paths,
    test_aliases_before_callbacks_or_writes,test_preparation_bad_text_arguments_and_old_bank_preserved,
    test_bank_binding_loss_latches_without_releasing_foreign_owner,test_cached_paint_never_reads_dataset_text_or_rows,
    test_maximum_text_is_owned_and_display_only_truncated,test_epoch_and_revision_exhaustion_never_reset,
    test_shared_core_oracle_mixed_owned_turns,test_capture_rasterize_publish_context_recheck,
    test_receiver_identity_is_fixed_before_first_publication,test_foreign_live_bank_and_observation_cannot_cross_owners,
    test_initial_authority_refusal_never_publishes_defaults};
  assert(c<sizeof(cases)/sizeof(cases[0]));cases[c]();
  printf("{\"ok\":true,\"hostBytes\":%zu,\"bankBytes\":%zu,\"workspaceBytes\":%zu}\n",sizeof(hosts[0]),sizeof(banks[0]),sizeof(work));return 0;
}
