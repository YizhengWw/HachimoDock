/* Private route-scoped downstream integrity negotiation, NOT authentication.
 * Only the single protocol worker negotiates/snapshots. Other trusted senders
 * may atomically read the framing bit. Route tokens are boot-owned addresses,
 * never dereferenced or supplied by a component. Audio/raw paths bypass this.
 */
#pragma once
#include "pet_p4_control_integrity.h"
#ifndef PET_P4_SERIAL_CONTROL_NEGOTIATED
#define PET_P4_SERIAL_CONTROL_NEGOTIATED 0
#endif
#if PET_P4_SERIAL_CONTROL_NEGOTIATED && (!PET_P4_SERIAL_CONTROL_INTEGRITY || !PET_P4_HELLO_ASYNC)
#error "Negotiated framing requires control integrity and serialized async HELLO"
#endif
#define PET_CONTROL_NONCE_BYTES 32U
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
typedef struct {bool supported,framed;char nonce[PET_CONTROL_NONCE_BYTES+1];} pet_control_session_snapshot_t;
bool pet_control_sessions_init(const void *uart,const void *jtag);
/* absent proposal resets ONLY this route to legacy. Invalid proposals leave
 * all state unchanged. Valid requests on unsupported native routes are ignored
 * without falsely advertising support. No key, payload, pointer or ID log. */
bool pet_control_session_negotiate(const void *route,bool proposed,const char *engine,const char *nonce);
struct cJSON;
bool pet_control_session_payload(const void *route,const struct cJSON *payload);
void pet_control_session_capabilities(const void *route,struct cJSON *capabilities);
bool pet_control_session_snapshot(const void *route,pet_control_session_snapshot_t *out);
bool pet_control_session_framed(const void *route);
#endif
