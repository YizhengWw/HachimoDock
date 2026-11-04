/* Private passive SDK call boundaries. No driver replacement, retries or
 * watchdog policy changes. Each original call executes exactly once. */
#include "pet_p4_reset_phase.h"
#include "pet_p4_flash_phase.h"
#if PET_P4_FLASH_PHASE_TRACE
#if !PET_P4_RESET_PHASE_TRACE
#error "Flash phases require the native task/RTC ownership contract"
#endif
#include <errno.h>
#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_idf_version.h"
#if !defined(PET_FLASH_PHASE_HOST_TEST) && (!defined(CONFIG_IDF_TARGET_ESP32P4) || (ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1) && \
    !(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4) && \
      CONFIG_ESP_REV_MIN_FULL == 300 && CONFIG_ESP_REV_MAX_FULL == 399 && !CONFIG_SECURE_ENABLE_TEE)))
#error "Audit exact SDK signatures and linker/vtable call coverage first"
#endif

/* Marker calls themselves refuse ISR/critical contexts. Preserve errno on
 * either side and propagate every native error; a phase is not a receipt.
 * IRAM wrappers do not turn the task-only recorder into an ISR callback. */
#define FLASH_CALL(name,result_type,phase,parameters,arguments) \
  result_type __real_##name parameters; \
  result_type IRAM_ATTR __wrap_##name parameters { \
    int before=errno; \
    pet_reset_phase_scope_t scope=pet_reset_phase_enter(phase); \
    errno=before;result_type result=__real_##name arguments;int after=errno; \
    pet_reset_phase_leave(&scope);errno=after;return result; \
  }
FLASH_CALL(esp_partition_erase_range,esp_err_t,PET_PHASE_PARTITION_ERASE,
  (const esp_partition_t *partition,size_t offset,size_t size),(partition,offset,size))
FLASH_CALL(esp_flash_erase_region,esp_err_t,PET_PHASE_FLASH_ERASE,
  (esp_flash_t *chip,uint32_t start,uint32_t length),(chip,start,length))
FLASH_CALL(spi_flash_chip_generic_yield,esp_err_t,PET_PHASE_FLASH_YIELD,
  (esp_flash_t *chip,uint32_t wip),(chip,wip))
FLASH_CALL(spi_flash_chip_generic_erase_sector,esp_err_t,PET_PHASE_FLASH_SECTOR,
  (esp_flash_t *chip,uint32_t address),(chip,address))
/* GD's selected driver aliases this Winbond implementation. It is a
 * different SDK unit; do not assume the generic sector wrapper covers it. */
FLASH_CALL(spi_flash_chip_winbond_erase_sector,esp_err_t,PET_PHASE_FLASH_SECTOR,
  (esp_flash_t *chip,uint32_t address),(chip,address))
FLASH_CALL(spi_flash_chip_generic_wait_idle,esp_err_t,PET_PHASE_FLASH_WAIT_IDLE,
  (esp_flash_t *chip,uint32_t timeout_us),(chip,timeout_us))
FLASH_CALL(spi_flash_check_and_flush_cache,bool,PET_PHASE_FLASH_CACHE_FLUSH,
  (size_t start,size_t length),(start,length))
#undef FLASH_CALL
#ifndef PET_FLASH_PHASE_HOST_TEST
#include "spi_flash_chip_driver.h"
#if ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4)
#include <stddef.h>
_Static_assert(sizeof(void *) == 4 && offsetof(esp_flash_t, chip_drv) == 4, "review flash adapter field layout");
_Static_assert(__builtin_types_compatible_p(__typeof__(((spi_flash_chip_t *)0)->erase_sector), esp_err_t (*)(esp_flash_t *, uint32_t)), "review sector vtable signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(((spi_flash_chip_t *)0)->wait_idle), esp_err_t (*)(esp_flash_t *, uint32_t)), "review wait vtable signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(((spi_flash_chip_t *)0)->yield), esp_err_t (*)(esp_flash_t *, uint32_t)), "review yield vtable signature");
/* OS start changed in 5.5.4. We neither wrap nor invoke it, but pin the reviewed
 * signature to prevent accidentally admitting another private SDK ABI. */
_Static_assert(__builtin_types_compatible_p(__typeof__(((esp_flash_os_functions_t *)0)->start), esp_err_t (*)(void *, uint32_t)), "review flash OS start signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&esp_partition_erase_range), esp_err_t (*)(const esp_partition_t *, size_t, size_t)), "review partition erase signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&esp_flash_erase_region), esp_err_t (*)(esp_flash_t *, uint32_t, uint32_t)), "review flash erase signature");
#endif
uint32_t pet_flash_phase_coverage(void){
  const esp_flash_t *chip=esp_flash_default_chip;
  if(!chip||!chip->chip_drv)return 0;
  const spi_flash_chip_t *driver=chip->chip_drv;
  return ((driver->erase_sector==__wrap_spi_flash_chip_generic_erase_sector
      ||driver->erase_sector==__wrap_spi_flash_chip_winbond_erase_sector)?1U:0)
    |(driver->wait_idle==__wrap_spi_flash_chip_generic_wait_idle?2U:0)
    |(driver->yield==__wrap_spi_flash_chip_generic_yield?4U:0);
}
#endif
#endif
