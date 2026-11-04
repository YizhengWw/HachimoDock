#include "pet_p4_native_panic.h"
#if PET_P4_NATIVE_PANIC_TRACE
#include <stddef.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
#if !defined(PET_NATIVE_PANIC_HOST_TEST) && (!defined(CONFIG_IDF_TARGET_ESP32P4) || (ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1) && \
    !(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4) && \
      CONFIG_ESP_REV_MIN_FULL == 300 && CONFIG_ESP_REV_MAX_FULL == 399 && !CONFIG_SECURE_ENABLE_TEE)))
#error "Review exact native panic/frame ABI and IRAM/RTC placement first"
#endif
#if !defined(PET_NATIVE_PANIC_HOST_TEST) && ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4)
_Static_assert(sizeof(void *) == 4 && sizeof(long) == 4, "review RV32 panic ABI");
_Static_assert(sizeof(panic_info_t) == 36 && offsetof(panic_info_t, core) == 0
    && offsetof(panic_info_t, exception) == 4 && offsetof(panic_info_t, frame) == 28
    && offsetof(panic_info_t, pseudo_excause) == 32, "review native panic info layout");
_Static_assert(sizeof(RvExcFrame) == 148 && offsetof(RvExcFrame, mepc) == 0
    && offsetof(RvExcFrame, ra) == 4 && offsetof(RvExcFrame, mcause) == 136, "review native exception frame layout");
#endif
/* Each already-stalled native core has its own first-entry record. No heap,
 * locks, logging, task API, stack words, reason strings or faulting data are
 * read here. Every SDK invocation still runs exactly once with its original
 * argument. All native watchdog/printing/reboot policy remains in the SDK. */
typedef struct {
  uint32_t magic,build[8],nonce;
  pet_native_panic_site_t site;
  uint32_t checksum,commit;
} panic_record_t;
_Static_assert(sizeof(panic_record_t)==72,"fixed RTC evidence ABI");
static RTC_NOINIT_ATTR panic_record_t panic_records[2];
static DRAM_ATTR pet_native_panic_snapshot_t previous_panic;
static DRAM_ATTR uint32_t panic_build[8],panic_nonce;
static DRAM_ATTR volatile bool panic_ready,panic_entered[2];
#define PANIC_MAGIC UINT32_C(0x504e4331)
#define PANIC_COMMIT UINT32_C(0x729bc5a6)
static uint32_t IRAM_ATTR panic_sum(const panic_record_t *record){
  uint32_t value=UINT32_C(2166136261);
  /* Numeric fields only, excludes checksum/commit; no structure padding. */
  const uint32_t *words=(const uint32_t*)record;
  for(unsigned i=0;i<offsetof(panic_record_t,checksum)/4;i++)value=(value^words[i])*UINT32_C(16777619);
  return value;
}
static void IRAM_ATTR panic_copy_from_rtc(const volatile panic_record_t *source,panic_record_t *out){
  const volatile uint32_t *s=(const volatile uint32_t*)source;uint32_t *d=(uint32_t*)out;
  for(unsigned i=0;i<sizeof(*out)/4;i++)d[i]=s[i];
}
void pet_native_panic_boot(void){
  if(panic_ready)return;
  const esp_app_desc_t *app=esp_app_get_description();if(!app)return;
  memcpy(panic_build,app->app_elf_sha256,sizeof(panic_build));
  esp_reset_reason_t why=esp_reset_reason();
  bool fault=why==ESP_RST_PANIC||why==ESP_RST_INT_WDT||why==ESP_RST_TASK_WDT||why==ESP_RST_WDT;
  for(unsigned core=0;core<2;core++){
    panic_record_t record;panic_copy_from_rtc(&panic_records[core],&record);
    bool valid=record.magic==PANIC_MAGIC&&record.nonce&&record.site.core==core
      &&record.commit==(record.nonce^PANIC_COMMIT)&&record.checksum==panic_sum(&record);
    if(fault&&valid&&!memcmp(record.build,panic_build,sizeof(panic_build))){
      previous_panic.same_build=true;previous_panic.sites[core]=record.site;previous_panic.valid_mask|=1U<<core;
    }
    /* Begin a new diagnostic epoch, NOT a fault-history reset. A partial new
     * record cannot authorize previous-boot evidence or a recovery action. */
    panic_records[core].commit=0;
  }
  panic_nonce=esp_random();if(!panic_nonce)panic_nonce=1;
  __sync_synchronize();panic_ready=true;
}
bool pet_native_panic_previous(pet_native_panic_snapshot_t *out){
  if(!out||!panic_ready)return false;
  *out=previous_panic;return true;
}
void __real_esp_panic_handler(panic_info_t *info);
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info){
  if(panic_ready&&info&&info->core>=0&&info->core<2&&info->frame){
    unsigned core=(unsigned)info->core;
    if(!panic_entered[core]){
      panic_entered[core]=true;
      const RvExcFrame *frame=info->frame;
      panic_record_t record={.magic=PANIC_MAGIC,.nonce=panic_nonce,
        .site={.pc=(uint32_t)frame->mepc,.return_pc=(uint32_t)frame->ra,
          .cause=(uint32_t)frame->mcause,.core=core,.exception=(uint32_t)info->exception,
          .flags=(g_panic_abort?1U:0U)|(info->pseudo_excause?2U:0U)}};
      for(unsigned i=0;i<8;i++)record.build[i]=panic_build[i];
      record.checksum=panic_sum(&record);record.commit=record.nonce^PANIC_COMMIT;
      volatile uint32_t *dest=(volatile uint32_t*)&panic_records[core];
      const uint32_t *src=(const uint32_t*)&record;
      panic_records[core].commit=0;__sync_synchronize();
      for(unsigned i=0;i<offsetof(panic_record_t,commit)/4;i++)dest[i]=src[i];
      __sync_synchronize();panic_records[core].commit=record.commit;
    }
  }
  __real_esp_panic_handler(info);
}
#endif
