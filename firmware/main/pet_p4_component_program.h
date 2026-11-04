/* Versioned data-only component program. Ordinary builds remain closed.
 * Parsing writes inactive caller-owned scratch only. It does not create an
 * owner, grant, task, native callback, peripheral effect or live VM instance. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pet_p4_component_features.h"
#include "pet_p4_program_host.h"
#include "pet_p4_program_timers.h"
struct cJSON;
#define PET_COMPONENT_PROGRAM_ENGINE "p4-component-program-v1"
#define PET_COMPONENT_PROGRAM_ENGINE_V2 "p4-component-program-v2"
#define PET_COMPONENT_PROGRAM_IMAGE_ENGINE "p4-integer-program-v1"
#define PET_COMPONENT_PROGRAM_IMAGE_ENGINE_V2 "p4-integer-program-v2"
#define PET_COMPONENT_PROGRAM_ACTIONS 24U
typedef struct {
  bool enabled;
  bool v2; /* p4-component-program-v2 declared (PET_P4_PROGRAM_V2 builds only) */
#if PET_COMPONENT_PROGRAM_ENABLED
  pet_program_image_t image;
  pet_program_ui_projection_t projection;
  pet_program_timer_spec_t timers[PET_PROGRAM_TIMERS];
  struct {char name[48];pet_program_event_t event;int8_t input[PET_PROGRAM_EVENT_WORDS];} actions[PET_COMPONENT_PROGRAM_ACTIONS];
  uint8_t action_count,timer_count;
#endif
} pet_component_program_config_t;
/* Root runtime is already bounded JSON, but this independently validates the
 * new descriptor and its variable/action ownership. Failure is never a reset. */
bool pet_component_program_parse(const struct cJSON *runtime,pet_component_program_config_t *out);
#if PET_COMPONENT_PROGRAM_ENABLED
/* Pure event capture. The parent holds the current model/lifecycle lock. */
bool pet_component_program_event(const pet_component_program_config_t *config,const char *action,
  const int32_t values[PET_PROGRAM_UI_VARIABLES],pet_program_event_t *out);
bool pet_component_program_seed_valid(const pet_component_program_config_t *config,const pet_program_snapshot_t *seed);
#endif
