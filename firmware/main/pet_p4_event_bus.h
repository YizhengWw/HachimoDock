/* Trusted volatile integer-event bus. No package/wire API or implied grants.
 * Native owner registrations and masks come from an audited parent policy,
 * never from component-supplied handles/pointers. Production PROGRAM/PERIPHERAL
 * quotas remain unchanged. This bus does not allocate a task or resource lease.
 * Serialize all calls and parent lifecycle changes on the same owning mutex.
 * Tokens/storage cannot move, be serialized, copied into a package or reset.
 */
#pragma once
#include "pet_p4_resources.h"
#include "pet_p4_program_runtime.h"
#define PET_EVENT_BUS_OWNERS 4U
#define PET_EVENT_BUS_QUEUE 4U
struct pet_event_bus;
typedef struct {
  const struct pet_event_bus *bus;
  uint64_t serial;
  uint8_t slot;
} pet_event_registration_t;
typedef struct {
  uint64_t publisher_generation,publisher_sequence;
  uint8_t topic;
  pet_program_event_t event;
} pet_event_message_t;
typedef struct pet_event_bus {
  struct {
    pet_resource_owner_t owner;
    pet_event_message_t queue[PET_EVENT_BUS_QUEUE];
    uint64_t serial,last_publish,last_delivery;
    uint32_t publish_mask,subscribe_mask;
    uint8_t head,count;
    bool live,stopped;
  } slots[PET_EVENT_BUS_OWNERS];
  uint64_t serial;
  uint32_t marker;
  bool inside;
} pet_event_bus_t;
typedef enum {
  PET_EVENT_OK,PET_EVENT_INVALID,PET_EVENT_BUSY,PET_EVENT_STALE,PET_EVENT_DENIED,
  PET_EVENT_BACKPRESSURE,PET_EVENT_IDLE,PET_EVENT_DELIVERED,PET_EVENT_STOPPED,
  PET_EVENT_SEQUENCE_ERROR,PET_EVENT_EXHAUSTED
} pet_event_bus_result_t;
typedef bool (*pet_event_owner_current_t)(void *context,pet_resource_owner_t owner);
typedef pet_program_runtime_result_t (*pet_event_enqueue_t)(void *context,
  pet_resource_owner_t receiver,uint64_t sequence,const pet_event_message_t *message);
typedef struct {
  uint8_t recipients,remaining;
  pet_program_runtime_result_t enqueue_result;
  bool offered,consumed;
} pet_event_diagnostic_t;
/* Fresh {0} storage only, once per native owner lifetime. */
bool pet_event_bus_init(pet_event_bus_t *bus);
/* Topic bits 0..31 represent topics 1..32. current/enqueue are approved bounded
 * native observers/local enqueues only, no IO, provider execution or mutation
 * of bus/arguments. Reentry is rejected before any callbacks/output writes.
 * All owner handles belong to the same resource broker; no cross-broker grants.
 * One live registration per exact native owner; stale slots require explicit
 * close, not automatic expiry/reset that might confuse retained handles.
 * Failure preserves output and registration state. A new output must be {0}. */
pet_event_bus_result_t pet_event_bus_open(pet_event_bus_t *bus,pet_resource_owner_t owner,
  uint32_t publish_mask,uint32_t subscribe_mask,pet_event_owner_current_t current,
  void *context,pet_event_registration_t *out);
/* Atomic fanout to the currently authorized matching subscribers. If even one
 * live queue is full, none receive the event and the publisher ID is unconsumed.
 * Accepted IDs never fan out twice in this registration, even with no audience.
 * A late subscription receives no replay. Payload unused words are zeroed.
 * No durable delivery/external side effects are promised across reboot. */
pet_event_bus_result_t pet_event_bus_publish(pet_event_bus_t *bus,pet_event_registration_t publisher,
  uint64_t sequence,unsigned topic,const pet_program_event_t *event,
  pet_event_owner_current_t current,void *context,pet_event_diagnostic_t *diagnostic);
/* Offers ONE queued message, never pumps a program/drains queues. Recheck exact
 * receiver ownership immediately before the approved local enqueue. OK consumes
 * one message; BUSY/FULL retains it. Any other outcome latches stopped, retaining
 * pending messages until explicit close, never guessing acceptance or replaying.
 * sequence is supplied by the receiver's shared native event sequencer, NOT the
 * publisher sequence, so separate senders cannot collide in a runtime queue.
 * Already accepted runtime events require the parent's normal cancel/revoke;
 * closing the bus never revokes a broker owner or frees physical resources.
 */
pet_event_bus_result_t pet_event_bus_deliver(pet_event_bus_t *bus,pet_event_registration_t receiver,
  uint64_t sequence,pet_event_owner_current_t current,void *current_context,
  pet_event_enqueue_t enqueue,void *enqueue_context,pet_event_diagnostic_t *diagnostic);
/* Explicit local retirement, allowed even after broker revocation. Clears only
 * this registration's volatile queue. Old tokens cannot close/relabel a new one.
 * Closing a publisher does not undo an already accepted publication to other
 * still-current receivers; their own lifecycle/close controls that delivery.
 */
pet_event_bus_result_t pet_event_bus_close(pet_event_bus_t *bus,pet_event_registration_t registration);
