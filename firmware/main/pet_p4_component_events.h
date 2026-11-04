/*
 * [Input] Package-declared event names, alarm declarations, consistent UTC/uptime clock snapshots and
 *         the component's private save text.
 * [Output] A bounded device-wide event bus (<=32 components x 8 queued names) and per-component
 *          alarm sets with backward-compatible relative and UTC private saves.
 * [Pos] p4-events-v1 portable core (Track C). No tasks, allocation, files or wire commands;
 *       the miniapp hooks own locking and delivery. Host-tested through ctypes.
 * [Sync] If this file changes, update firmware/protocol.md and pc/skills/petui/references/events.md.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PET_CEV_NAME_MAX 16u          /* <=15 ASCII [A-Za-z0-9_.-] + NUL */
#define PET_CEV_NAMES_PER_PACKAGE 8u  /* emit names and subscriptions, each */
#define PET_CEV_QUEUE 8u              /* queued deliveries per component */
#define PET_CEV_COMPONENTS 32u        /* maxInstalled */
#define PET_CEV_WIDGET_ID_MAX 48u
#define PET_CEV_ALARMS 4u
#define PET_CEV_ALARM_MIN_MS 60000u
#define PET_CEV_ALARM_TEXT_MAX 41u
#define PET_CEV_ACTION_MAX 48u
#define PET_CEV_SAVE_TEXT_MAX 256u
typedef struct {
  char widget_id[PET_CEV_WIDGET_ID_MAX];
  char names[PET_CEV_NAMES_PER_PACKAGE][PET_CEV_NAME_MAX]; /* subscriptions */
  uint8_t name_count;
  char queue[PET_CEV_QUEUE][PET_CEV_NAME_MAX];
  uint8_t head, count;
  uint32_t dropped;
  bool used;
} pet_cev_component_t;
typedef struct {
  pet_cev_component_t items[PET_CEV_COMPONENTS];
  uint32_t emitted, delivered, dropped;
} pet_cev_bus_t;
bool pet_cev_name_valid(const char *name);
void pet_cev_bus_reset(pet_cev_bus_t *bus);
/* Registers (or replaces) the subscription list of one installed component. The
 * queue survives a re-registration with the same id; false when 32 are used. */
bool pet_cev_bus_subscribe(pet_cev_bus_t *bus, const char *widget_id, const char (*names)[PET_CEV_NAME_MAX], unsigned count);
void pet_cev_bus_unsubscribe(pet_cev_bus_t *bus, const char *widget_id);
/* Fans one name out to every subscribed component (the emitter included when it
 * subscribes). Full queues drop and count; returns the number of deliveries. */
unsigned pet_cev_bus_emit(pet_cev_bus_t *bus, const char *name);
bool pet_cev_bus_pop(pet_cev_bus_t *bus, const char *widget_id, char out[PET_CEV_NAME_MAX]);
unsigned pet_cev_bus_pending(const pet_cev_bus_t *bus, const char *widget_id);
uint32_t pet_cev_bus_dropped(const pet_cev_bus_t *bus, const char *widget_id);

/* utc_ms is extrapolated UTC at the caller's now_ms. All fields are captured
 * under one clock lock; timezone offsets never enter this value. */
typedef struct {
  bool valid;
  uint64_t utc_ms, monotonic_at_sync, calibration_generation;
} pet_cev_clock_snapshot_t;
typedef struct {
  char id[PET_CEV_NAME_MAX];
  char action[PET_CEV_ACTION_MAX];
  char text[PET_CEV_ALARM_TEXT_MAX];
  uint32_t every_ms;      /* periodic when >= 60000, else one-shot driven by at_var */
  int8_t at_var;          /* int var index holding a delay in ms, or -1 */
  int32_t last_var;       /* last value observed from at_var (re-arm on change) */
  uint64_t deadline_ms;   /* uptime deadline while armed */
  bool background, armed, pending;
  uint64_t utc_deadline_ms;
  uint32_t waiting_ms;
  bool utc_deadline, waiting_for_clock, cancelled;
} pet_cev_alarm_t;
typedef struct {
  pet_cev_alarm_t items[PET_CEV_ALARMS];
  uint8_t count;
  uint32_t clock_changes; /* transient change observation, never authority */
} pet_cev_alarm_set_t;
/* Arms every periodic alarm from now; one-shots stay disarmed until their var is seen. */
/* Explicit cancellation persists; periodic start does not undo cancellation. */
void pet_cev_alarms_cancel(pet_cev_alarm_set_t *set, unsigned index);
void pet_cev_alarms_start(pet_cev_alarm_set_t *set, uint64_t now_ms);
/* One-shot (at_var) alarms: value >= 60000 arms now+value when it differs from the
 * last seen value; 0 disarms. Values below the floor are ignored. */
void pet_cev_alarms_observe_var(pet_cev_alarm_set_t *set, unsigned index, int32_t value, uint64_t now_ms);
/* Marks due alarms pending (periodic ones re-arm). Returns the bitmask that became due. */
unsigned pet_cev_alarms_tick(pet_cev_alarm_set_t *set, uint64_t now_ms);
/* Returns and clears the pending bitmask. */
unsigned pet_cev_alarms_take_pending(pet_cev_alarm_set_t *set);
/* Private-save text "id:remaining_ms[:p][:v<last_var>][:c];...": remaining
 * duration, never a boot uptime or Unix epoch. Powered-off time does not elapse.
 * v preserves one-shot observation; c persists periodic cancellation. Legacy
 * saves without v baseline their first restored variable without re-arming.
 * Returns bytes written, or zero with empty output if capacity cannot fit all.
 * Absolute wall-clock deadlines are not declared by this schema. */
size_t pet_cev_alarms_serialize(const pet_cev_alarm_set_t *set, uint64_t now_ms, char *out, size_t cap);
/* Restores by id; unknown ids are ignored, malformed text returns false. */
bool pet_cev_alarms_restore(pet_cev_alarm_set_t *set, const char *text, uint64_t now_ms);

/* Persistent clock path. Old entrypoints/relative saves remain supported.
 * Unknown UTC preserves WAITING_FOR_CLOCK, never compares uptime to UTC. */
void pet_cev_alarms_start_clock(pet_cev_alarm_set_t*,uint64_t,const pet_cev_clock_snapshot_t*);
void pet_cev_alarms_observe_var_clock(pet_cev_alarm_set_t*,unsigned,int32_t,uint64_t,const pet_cev_clock_snapshot_t*);
unsigned pet_cev_alarms_tick_clock(pet_cev_alarm_set_t*,uint64_t,const pet_cev_clock_snapshot_t*);
/* :u marks an absolute UTC ms deadline; :w a delay awaiting first calibration.
 * Optional :p/:v<last_var>/:c preserve delivery, observation and cancellation.
 * Reader is two-pass/transactional, with no format-sized local state copy. */
size_t pet_cev_alarms_serialize_clock(const pet_cev_alarm_set_t*,uint64_t,const pet_cev_clock_snapshot_t*,char*,size_t);
bool pet_cev_alarms_restore_clock(pet_cev_alarm_set_t*,const char*,uint64_t,const pet_cev_clock_snapshot_t*);
