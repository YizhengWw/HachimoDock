#include "pet_p4_collection_host.h"
#include "pet_p4_collection_internal.h"
#include <limits.h>
#include <string.h>
#define DATASET_MARKER UINT32_C(0x50344344)
#define HOST_MARKER UINT32_C(0x50344348)
static bool overlap(const void*a,size_t an,const void*b,size_t bn){
  if(!a||!b)return false;
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
static bool identity_valid(const uint8_t identity[PET_COLLECTION_IDENTITY]){
  if(!identity)return false;
  unsigned any=0;for(unsigned i=0;i<PET_COLLECTION_IDENTITY;i++)any|=identity[i];return any!=0;
}
static bool bounded_length(const char*s,size_t limit,size_t*out){
  if(!s)return false;
  for(size_t i=0;i<=limit;i++)if(!s[i]){*out=i;return true;}
  return false;
}
static bool aliases(const pet_collection_host_t*h,const void*p,size_t n){
  return overlap(h,sizeof(*h),p,n)||(h&&h->marker==HOST_MARKER&&overlap(h->dataset,sizeof(*h->dataset),p,n));
}
static pet_collection_host_result_t core_result(pet_collection_result_t r){
  switch(r){
    case PET_COLLECTION_OK:return PET_COLLECTION_HOST_OK;
    case PET_COLLECTION_BAD_DATA:return PET_COLLECTION_HOST_BAD_DATA;
    case PET_COLLECTION_BAD_STATE:return PET_COLLECTION_HOST_BAD_STATE;
    case PET_COLLECTION_NO_ROW:return PET_COLLECTION_HOST_NO_ROW;
    default:return PET_COLLECTION_HOST_INVALID;
  }
}
pet_collection_host_result_t pet_collection_dataset_prepare(pet_collection_dataset_t*out,
    const uint8_t identity[PET_COLLECTION_IDENTITY],uint64_t revision,
    const pet_collection_row_t*rows,unsigned count,pet_collection_host_workspace_t*work){
  if(!out||!work||!revision||count>PET_COLLECTION_ROWS||(!rows&&count)||!identity
      ||overlap(out,sizeof(*out),work,sizeof(*work))||overlap(out,sizeof(*out),identity,PET_COLLECTION_IDENTITY)
      ||overlap(out,sizeof(*out),rows,count*sizeof(*rows))||overlap(work,sizeof(*work),identity,PET_COLLECTION_IDENTITY)
      ||overlap(work,sizeof(*work),rows,count*sizeof(*rows)))return PET_COLLECTION_HOST_INVALID;
  if(out->bound||(out->marker&&out->marker!=DATASET_MARKER)||!identity_valid(identity))return PET_COLLECTION_HOST_INVALID;
  for(unsigned i=0;i<count;i++){
    const char*fields[]={rows[i].key,rows[i].label,rows[i].detail};
    for(unsigned f=0;f<3;f++){
      size_t n;if(!bounded_length(fields[f],f?PET_COLLECTION_SOURCE_TEXT:PET_COLLECTION_TEXT-1,&n))return PET_COLLECTION_HOST_BAD_DATA;
      if(overlap(out,sizeof(*out),fields[f],n+1)||overlap(work,sizeof(*work),fields[f],n+1))return PET_COLLECTION_HOST_INVALID;
    }
  }
  work->state=(pet_collection_state_t){0};
  pet_collection_result_t r=pet_collection_project(rows,count,PET_COLLECTION_VISIBLE,PET_COLLECTION_STATUS,&work->state,&work->view,&work->core);
  if(r!=PET_COLLECTION_OK)return core_result(r);
  /* No rejection/callback after writing begins. Source is immutable for call. */
  memset(out,0,sizeof(*out));
  for(unsigned i=0;i<count;i++){
    memcpy(out->records[i].key,rows[i].key,strlen(rows[i].key)+1);
    memcpy(out->records[i].label,rows[i].label,strlen(rows[i].label)+1);
    memcpy(out->records[i].detail,rows[i].detail,strlen(rows[i].detail)+1);
    out->rows[i]=(pet_collection_row_t){out->records[i].key,out->records[i].label,out->records[i].detail};
  }
  memcpy(out->identity,identity,PET_COLLECTION_IDENTITY);out->revision=revision;out->count=(uint16_t)count;out->marker=DATASET_MARKER;
  return PET_COLLECTION_HOST_OK;
}
bool pet_collection_host_init(pet_collection_host_t*h,const pet_collection_authority_t*a){
  if(!h||!a||!a->generation||!a->current||!identity_valid(a->identity)||overlap(h,sizeof(*h),a,sizeof(*a)))return false;
  if(h->marker||h->authority.current||h->authority.context||h->authority.generation
      ||identity_valid(h->authority.identity)||h->dataset||h->epoch||h->revision||h->inside||h->stopped||h->published)return false;
  memset(h,0,sizeof(*h));h->authority=*a;memcpy(h->identity,a->identity,PET_COLLECTION_IDENTITY);h->marker=HOST_MARKER;return true;
}
static void retire(pet_collection_host_t*h){
  h->stopped=true;
  if(h->dataset&&h->dataset->bound==h)h->dataset->bound=NULL;
  h->dataset=NULL;
}
static pet_collection_host_result_t leave(pet_collection_host_t*h,pet_collection_host_result_t r){h->inside=false;return r;}
static bool current(pet_collection_host_t*h){
  if(h->authority.current(h->authority.context,h->authority.generation))return true;
  retire(h);return false;
}
static pet_collection_host_result_t enter(pet_collection_host_t*h,uint64_t generation,bool live){
  if(!h||h->marker!=HOST_MARKER)return PET_COLLECTION_HOST_INVALID;
  if(h->inside)return PET_COLLECTION_HOST_BUSY;
  if(!generation||generation!=h->authority.generation)return PET_COLLECTION_HOST_STALE;
  if(live&&h->stopped)return PET_COLLECTION_HOST_STOPPED;
  h->inside=true;
  if(live&&!current(h))return leave(h,PET_COLLECTION_HOST_DENIED);
  if(live&&h->published&&(!h->dataset||h->dataset->bound!=h)){
    retire(h);return leave(h,PET_COLLECTION_HOST_BAD_STATE);
  }
  return PET_COLLECTION_HOST_OK;
}
static bool bank_valid(const pet_collection_dataset_t*d){
  if(!d||d->marker!=DATASET_MARKER||!d->revision||d->count>PET_COLLECTION_ROWS||!identity_valid(d->identity))return false;
  for(unsigned i=0;i<d->count;i++)if(d->rows[i].key!=d->records[i].key||d->rows[i].label!=d->records[i].label||d->rows[i].detail!=d->records[i].detail)return false;
  return true;
}
pet_collection_host_result_t pet_collection_host_publish(pet_collection_host_t*h,uint64_t generation,
    pet_collection_dataset_t*d,const pet_collection_state_t*seed,unsigned visible,pet_collection_host_workspace_t*work){
  if(!d||!work||!visible||visible>PET_COLLECTION_VISIBLE||aliases(h,work,sizeof(*work))
      ||aliases(h,d,sizeof(*d))||overlap(d,sizeof(*d),work,sizeof(*work))
      ||(seed&&(aliases(h,seed,sizeof(*seed))||overlap(d,sizeof(*d),seed,sizeof(*seed))||overlap(work,sizeof(*work),seed,sizeof(*seed)))))return PET_COLLECTION_HOST_INVALID;
  pet_collection_host_result_t status=enter(h,generation,true);if(status!=PET_COLLECTION_HOST_OK)return status;
  if(!bank_valid(d)||d->bound)return leave(h,PET_COLLECTION_HOST_BAD_DATA);
  if(memcmp(h->identity,d->identity,PET_COLLECTION_IDENTITY)||(h->published&&d->revision<=h->revision))return leave(h,PET_COLLECTION_HOST_STALE);
  if(!h->published&&!seed)return leave(h,PET_COLLECTION_HOST_BAD_STATE);
  if(h->epoch==UINT64_MAX)return leave(h,PET_COLLECTION_HOST_EXHAUSTED);
  work->state=seed?*seed:h->state;
  pet_collection_result_t r=pet_collection_project_validated(d->rows,d->count,visible,PET_COLLECTION_STATUS,&work->state,&work->view,&work->core);
  if(r!=PET_COLLECTION_OK)return leave(h,core_result(r));
  if(!current(h))return leave(h,PET_COLLECTION_HOST_DENIED);
  /* Native parent lock covers this entire publication; no callbacks afterward. */
  if(h->dataset&&h->dataset->bound==h)h->dataset->bound=NULL;
  d->bound=h;h->dataset=d;h->state=work->state;h->view=work->view;
  h->revision=d->revision;h->epoch++;h->published=true;
  return leave(h,PET_COLLECTION_HOST_OK);
}
pet_collection_host_result_t pet_collection_host_read(pet_collection_host_t*h,uint64_t generation,
    pet_collection_view_t*out,pet_collection_observation_t*observation){
  if(!out||!observation||aliases(h,out,sizeof(*out))||aliases(h,observation,sizeof(*observation))
      ||overlap(out,sizeof(*out),observation,sizeof(*observation)))return PET_COLLECTION_HOST_INVALID;
  pet_collection_host_result_t r=enter(h,generation,true);if(r!=PET_COLLECTION_HOST_OK)return r;
  if(!h->published||!h->dataset||h->dataset->bound!=h)return leave(h,PET_COLLECTION_HOST_UNPUBLISHED);
  pet_collection_observation_t next={h,generation,h->epoch,h->revision};
  if(!current(h))return leave(h,PET_COLLECTION_HOST_DENIED);
  *out=h->view;*observation=next;return leave(h,PET_COLLECTION_HOST_OK);
}
pet_collection_host_result_t pet_collection_host_observation_current(pet_collection_host_t*h,uint64_t generation,
    const pet_collection_observation_t*observed){
  if(!observed||aliases(h,observed,sizeof(*observed)))return PET_COLLECTION_HOST_INVALID;
  pet_collection_host_result_t r=enter(h,generation,true);if(r!=PET_COLLECTION_HOST_OK)return r;
  if(!h->published)return leave(h,PET_COLLECTION_HOST_UNPUBLISHED);
  bool same=observed->host==h&&observed->generation==generation&&observed->epoch==h->epoch&&observed->revision==h->revision;
  return leave(h,same?PET_COLLECTION_HOST_OK:PET_COLLECTION_HOST_STALE);
}
pet_collection_host_result_t pet_collection_host_apply(pet_collection_host_t*h,uint64_t generation,
    pet_collection_operation_t op,const pet_collection_observation_t*observed,pet_collection_host_workspace_t*work){
  if(!observed||!work||op<=PET_COLLECTION_STATUS||op>PET_COLLECTION_ROW_7
      ||aliases(h,observed,sizeof(*observed))||aliases(h,work,sizeof(*work))
      ||overlap(observed,sizeof(*observed),work,sizeof(*work)))return PET_COLLECTION_HOST_INVALID;
  pet_collection_host_result_t r=enter(h,generation,true);if(r!=PET_COLLECTION_HOST_OK)return r;
  if(!h->published||!h->dataset||h->dataset->bound!=h)return leave(h,PET_COLLECTION_HOST_UNPUBLISHED);
  if(observed->host!=h||observed->generation!=generation||observed->epoch!=h->epoch||observed->revision!=h->revision)return leave(h,PET_COLLECTION_HOST_STALE);
  if(h->epoch==UINT64_MAX)return leave(h,PET_COLLECTION_HOST_EXHAUSTED);
  work->state=h->state;
  pet_collection_result_t result=pet_collection_project_validated(h->dataset->rows,h->dataset->count,h->view.visible,op,&work->state,&work->view,&work->core);
  if(result!=PET_COLLECTION_OK)return leave(h,core_result(result));
  if(!current(h))return leave(h,PET_COLLECTION_HOST_DENIED);
  h->state=work->state;h->view=work->view;h->epoch++;
  return leave(h,PET_COLLECTION_HOST_OK);
}
pet_collection_host_result_t pet_collection_host_snapshot(pet_collection_host_t*h,uint64_t generation,pet_collection_state_t*out){
  if(!out||aliases(h,out,sizeof(*out)))return PET_COLLECTION_HOST_INVALID;
  pet_collection_host_result_t r=enter(h,generation,false);if(r!=PET_COLLECTION_HOST_OK)return r;
  if(!h->published)return leave(h,PET_COLLECTION_HOST_UNPUBLISHED);
  *out=h->state;return leave(h,PET_COLLECTION_HOST_OK);
}
pet_collection_host_result_t pet_collection_host_cancel(pet_collection_host_t*h,uint64_t generation){
  pet_collection_host_result_t r=enter(h,generation,false);if(r!=PET_COLLECTION_HOST_OK)return r;
  retire(h);return leave(h,PET_COLLECTION_HOST_OK);
}
