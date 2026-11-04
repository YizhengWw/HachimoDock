#include "pet_p4_control_session.h"
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
#include <stdatomic.h>
#include <string.h>
#include "cJSON.h"
_Static_assert(ATOMIC_BOOL_LOCK_FREE==2,"framing observation must be lock-free");
typedef struct {const void *route;_Atomic bool framed;char nonce[PET_CONTROL_NONCE_BYTES+1];} session_t;
static session_t g_control_route_sessions[2];
static bool g_control_route_sessions_ready;
bool pet_control_sessions_init(const void *uart,const void *jtag){
  if(g_control_route_sessions_ready||!uart||!jtag||uart==jtag)return false;
  g_control_route_sessions[0].route=uart;g_control_route_sessions[1].route=jtag;
  g_control_route_sessions_ready=true;return true;
}
static session_t *find(const void *route){
  if(g_control_route_sessions_ready&&route)for(unsigned i=0;i<2;i++)if(g_control_route_sessions[i].route==route)return &g_control_route_sessions[i];
  return NULL;
}
static bool nonce_valid(const char *nonce){
  if(!nonce)return false;
  for(unsigned i=0;i<PET_CONTROL_NONCE_BYTES;i++)
    if(!((nonce[i]>='0'&&nonce[i]<='9')||(nonce[i]>='a'&&nonce[i]<='f')))return false;
  return nonce[PET_CONTROL_NONCE_BYTES]==0;
}
bool pet_control_session_negotiate(const void *route,bool proposed,const char *engine,const char *nonce){
  if(!g_control_route_sessions_ready)return false;
  if(proposed&&(!engine||strcmp(engine,PET_CONTROL_INTEGRITY_ENGINE)||!nonce_valid(nonce)))return false;
  session_t *s=find(route);if(!s)return true; /* Unsupported route stays plain. */
  if(proposed)memcpy(s->nonce,nonce,PET_CONTROL_NONCE_BYTES+1);else memset(s->nonce,0,sizeof(s->nonce));
  atomic_store_explicit(&s->framed,proposed,memory_order_release);return true;
}
bool pet_control_session_payload(const void *route,const cJSON *payload){
  if(!cJSON_IsObject(payload))return false;
  const cJSON *proposal=NULL,*engine=NULL,*nonce=NULL,*member=NULL;unsigned proposals=0,fields=0;
  cJSON_ArrayForEach(member,payload)if(member->string&&!strcmp(member->string,"serialControl")){proposal=member;proposals++;}
  bool valid=proposals<=1;
  if(proposal){
    valid=valid&&cJSON_IsObject(proposal);
    cJSON_ArrayForEach(member,proposal){
      fields++;
      if(member->string&&!strcmp(member->string,"engine")&&!engine)engine=member;
      else if(member->string&&!strcmp(member->string,"nonce")&&!nonce)nonce=member;
      else valid=false;
    }
    valid=valid&&fields==2&&cJSON_IsString(engine)&&cJSON_IsString(nonce);
  }
  return valid&&pet_control_session_negotiate(route,proposal!=NULL,
    engine?engine->valuestring:NULL,nonce?nonce->valuestring:NULL);
}
bool pet_control_session_snapshot(const void *route,pet_control_session_snapshot_t *out){
  if(!out||!g_control_route_sessions_ready)return false;
  pet_control_session_snapshot_t value={0};session_t *s=find(route);
  if(s){value.supported=true;value.framed=atomic_load_explicit(&s->framed,memory_order_acquire);
    if(value.framed)memcpy(value.nonce,s->nonce,sizeof(value.nonce));}
  *out=value;return true;
}
bool pet_control_session_framed(const void *route){
  session_t *s=find(route);return s&&atomic_load_explicit(&s->framed,memory_order_acquire);
}
void pet_control_session_capabilities(const void *route,cJSON *capabilities){
  pet_control_session_snapshot_t session={0};
  if(!cJSON_IsObject(capabilities)||!pet_control_session_snapshot(route,&session)||!session.supported)return;
  cJSON_AddStringToObject(capabilities,"serialControlOffer",PET_CONTROL_INTEGRITY_ENGINE);
  cJSON_AddNumberToObject(capabilities,"serialControlMaxBytes",PET_CONTROL_INTEGRITY_MAX);
  if(session.framed){
    cJSON_AddStringToObject(capabilities,"serialControlIntegrity",PET_CONTROL_INTEGRITY_ENGINE);
    cJSON_AddStringToObject(capabilities,"serialControlNonce",session.nonce);
  }
}
#endif
