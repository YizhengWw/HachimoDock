/* Trusted diagnostic-only entry. Not a package service or bytecode loader.
 * Call only from the protocol worker, never the UI/audio task. Sends one bounded
 * result to the caller's route; does not broadcast, retry or reset user state. */
#pragma once
#include <stdbool.h>
typedef void (*pet_program_probe_send_fn)(const char *line, void *context);
void pet_p4_program_probe(pet_program_probe_send_fn send, void *context);
void pet_p4_program_publication_probe(pet_program_probe_send_fn send,void *context);
void pet_p4_program_session_probe(pet_program_probe_send_fn send,void *context);
/* Asynchronous FIXED native diagnostic. Retained callback/context must outlive
 * terminal completion (only static native routes on the wire entry). Process
 * once per main turn outside UI locks; never call from audio or an ISR. */
void pet_p4_program_main_probe(pet_program_probe_send_fn send,void *context);
bool pet_p4_program_main_probe_bind_owner(void);
void pet_p4_program_main_probe_process(void);
/* Protocol-worker only: delivery may drain UART. Main only publishes readiness.
 * While pending, the worker uses a bounded queue wait to collect completion. */
bool pet_p4_program_main_probe_pending(void);
void pet_p4_program_main_probe_reply(void);
