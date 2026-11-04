#include "pet_p4_fs_activity.h"
#include "freertos/FreeRTOS.h"
#include <errno.h>

static portMUX_TYPE fs_activity_mux=portMUX_INITIALIZER_UNLOCKED;
static uint64_t fs_activity_epoch;
static uint32_t fs_activity_active;
static bool fs_activity_enabled,fs_activity_unsafe;
static void fs_activity_restore_errno(int *saved){errno=*saved;}
#define PRESERVE_FS_ERRNO int saved_fs_errno __attribute__((cleanup(fs_activity_restore_errno)))=errno
static void fs_activity_advance(void){
  if(fs_activity_epoch==UINT64_MAX)fs_activity_unsafe=true;
  else fs_activity_epoch++;
}
pet_fs_activity_t pet_fs_activity_begin(void){
  PRESERVE_FS_ERRNO;
  pet_fs_activity_t token={0};
  portENTER_CRITICAL(&fs_activity_mux);
  if(fs_activity_active==UINT32_MAX)fs_activity_unsafe=true;
  else{fs_activity_active++;token.counted=true;}
  fs_activity_advance();
  portEXIT_CRITICAL(&fs_activity_mux);
  return token;
}
void pet_fs_activity_end(pet_fs_activity_t *token){
  PRESERVE_FS_ERRNO;
  if(!token||!token->counted)return;
  portENTER_CRITICAL(&fs_activity_mux);
  token->counted=false;fs_activity_active--;fs_activity_advance();
  portEXIT_CRITICAL(&fs_activity_mux);
}
bool pet_fs_activity_idle_epoch(uint64_t *epoch){
  PRESERVE_FS_ERRNO;
  if(!epoch)return false;
  portENTER_CRITICAL(&fs_activity_mux);
  bool idle=fs_activity_enabled&&!fs_activity_unsafe&&!fs_activity_active;
  *epoch=fs_activity_epoch;
  portEXIT_CRITICAL(&fs_activity_mux);
  return idle;
}
void pet_fs_activity_enable_observation(void){
  PRESERVE_FS_ERRNO;
  portENTER_CRITICAL(&fs_activity_mux);
  if(!fs_activity_enabled){fs_activity_enabled=true;fs_activity_advance();}
  portEXIT_CRITICAL(&fs_activity_mux);
}
