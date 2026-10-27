#pragma once
#include "pet_p4_miniapp.h"
#include "pet_p4_protocol.h"
/* One active, ephemeral host dashboard. No private data is written to Flash. */
void pet_app_host_transport(pet_p4_send_line_fn send,void *ctx,const char *board);
void pet_app_host_tick(const char *id,uint32_t revision,uint8_t page,bool visible,uint64_t now);
bool pet_app_host_action(const char *id,uint32_t revision,uint8_t page,const char *action,uint64_t now);
bool pet_app_host_confirmed_action(const char *id,uint32_t revision,uint8_t page,const char *action,const char *context,uint64_t now);
int pet_app_host_mode(const char *id,uint64_t now);
bool pet_app_host_enabled(const char *id,const char *action,uint64_t now);
void pet_app_host_context(const char *id,char context[65]);
bool pet_app_host_apply(const cJSON *payload,uint64_t now);
void pet_app_host_view(pet_p4_miniapp_view_t *out,uint64_t now);
