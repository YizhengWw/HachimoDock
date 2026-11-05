
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include "cJSON.h"
#include "pet_p4_assets.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_appearance_session.h"
#define APPEARANCE_OBSERVATION_SCHEMA "p4-appearance-quiescent-v1"
#define PET_P4_ASSET_SLOT_COUNT 2
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_MUTATION_ACK_DEFERRED 0
#define PET_RESOURCE_STORAGE_WORK 1
#define PET_RESOURCE_MASK(n) (1u<<(n))
#define PET_RESOURCE_OK 0
#define PET_RESOURCE_BUSY 3
typedef int pet_resource_result_t;
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {pet_p4_asset_catalog_t asset_catalog;char board_device_id[64];bool asset_transfer_active;unsigned asset_revision;} pet_p4_runtime_state_t;
typedef struct {int id;} pet_resource_owner_t;
typedef struct {int id;} pet_resource_lease_t;
typedef struct {bool transfer_active,receiving,worker_pending;uint32_t epoch;}pet_p4_native_io_snapshot_t;
static pet_appearance_session_t g_appearance_session;
static bool held=true,forbid_storage=true,storage_busy,storage_unavailable,refuse_callback,change_identity;
static unsigned disk_reads,snapshots,broker_calls,callbacks,sends;
static pet_p4_runtime_state_t state;
static char boot[128]="0000000000000001";
static cJSON *captured;
static _Atomic int g_asset_active_slot=0;
static FILE *g_asset_chunk_file;
static int g_asset_transfer_slot=-1;
static struct {bool active;} *private_tracking;
#define g_raw_asset_chunk (*private_tracking)
static bool (*g_asset_query_io_begin)(void *);
static void (*g_asset_query_io_end)(void *);
static void *g_asset_query_io_context;
static bool (*g_asset_query_pending)(void *);
static void *g_asset_query_pending_context;
static const char *pet_p4_boot_identity(void){return boot;}
static bool io_begin(void *p){assert(held);callbacks++;if(refuse_callback)return false;held=false;if(change_identity){strcpy(state.board_device_id,"new-board");strcpy(boot,"0000000000000002");}return true;}
static void io_end(void *p){assert(!held);held=true;}
static bool pending(void*p){assert(held&&!forbid_storage);return false;}
static pet_p4_native_io_snapshot_t pet_p4_native_usb_io_snapshot(void){assert(held&&!forbid_storage);snapshots++;return (pet_p4_native_io_snapshot_t){.epoch=9};}
static bool pet_p4_miniapp_storage_busy(void){assert(held&&!forbid_storage);return false;}
static bool pet_p4_ota_transfer_active(void){assert(held&&!forbid_storage);return false;}
static bool pet_p4_diagnostics_reboot_pending(void){assert(held&&!forbid_storage);return false;}
static int pet_resources_device_owner_open(unsigned mask,pet_resource_owner_t *owner){assert(held&&!forbid_storage);broker_calls++;owner->id=1;return 0;}
static int pet_resources_device_acquire(pet_resource_owner_t owner,int kind,int n,pet_resource_lease_t *lease){assert(held&&!forbid_storage);broker_calls++;lease->id=2;return storage_busy?PET_RESOURCE_BUSY:storage_unavailable?-1:0;}
static void pet_resources_device_revoke(pet_resource_owner_t owner){assert(held&&!forbid_storage);}
static int pet_resources_device_finish(pet_resource_lease_t lease){assert(held&&!forbid_storage);return 0;}
static bool pet_resources_device_current(pet_resource_lease_t lease){assert(held&&!forbid_storage);return true;}
/* SDK storage boundary: real private temporary files; query assembly is actual
 * production C. Not a test of the independent raw-header validator. */
static bool read_ready_slot_pack_id(int slot,char *pack,size_t cap){
 assert(!held&&!forbid_storage);disk_reads++;
 char path[32];snprintf(path,sizeof(path),"slot-%d.synthetic",slot);FILE*f=fopen(path,"rb");assert(f);
 size_t n=fread(pack,1,cap-1,f);assert(!ferror(f)&&fclose(f)==0);pack[n]=0;return n==64;
}
static void capture(const char*line,void *route){
 assert(!held&&route==&state);sends++;assert(sends==1);
 cJSON*root=cJSON_Parse(line);assert(root);assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root,"topic")->valuestring,"asset/ack"));
 captured=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(root,"payload"),true);cJSON_Delete(root);assert(captured);
}

static unsigned allocations;static int allocation_budget=-1;
static void *tracked_alloc(size_t n){if(allocation_budget==0)return NULL;if(allocation_budget>0)allocation_budget--;void*p=malloc(n);if(p)allocations++;return p;}
static void tracked_free(void *p){if(p){assert(allocations);allocations--;free(p);}}

/* Audio worker is outside this appearance-only fixture. Unexpected audio
 * mutation paths fail rather than fabricate a receipt. */
void pet_p4_audio_status_cues_defer(bool busy){(void)busy;}
bool pet_p4_raw_assets_stat_for_slot(int slot,const char*pack,const char*path,size_t expected,uint32_t*size,unsigned long long*checksum){(void)slot;(void)pack;(void)path;(void)expected;(void)size;(void)checksum;assert(!"empty fixture catalog cannot request a video proof");return false;}

typedef struct {char path[PET_P4_ASSET_PATH_MAX];uint32_t size;} asset_query_video_t;
static asset_query_video_t g_asset_query_videos[PET_P4_ASSET_CATALOG_MAX_FAMILIES];
static unsigned g_asset_query_video_count;
