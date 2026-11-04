/* Bounded typed local forms. This is product code, never a package executable.
 * The trusted parser owns/validates immutable form+text configuration once;
 * calls are serialized with lifecycle and state publication. No heap/IO/audio.
 * Native input/output objects must not overlap immutable configuration. */
#pragma once
#include "pet_p4_app_form.h"
#ifndef PET_FORM_V2_ENABLED
#define PET_FORM_V2_ENABLED 0
#endif
#define PET_FORM_CHARACTERS 96U
enum {PET_FORM_APPEND=6,PET_FORM_ERASE=7,PET_FORM_V2_ACTIONS=8};
typedef struct {int32_t number;char text[64];} pet_form_value_t;
typedef struct {
  bool enabled;
  uint8_t text_mask,max_bytes[PET_FORM_FIELDS],index_var,label_var,character_count;
  char characters[PET_FORM_CHARACTERS][5],space_label[17],extra_actions[2][48];
} pet_form_text_config_t;
typedef enum {PET_FORM_REPLACE_OK,PET_FORM_REPLACE_UNCHANGED,PET_FORM_REPLACE_DENIED,
  PET_FORM_REPLACE_INVALID,PET_FORM_REPLACE_LIMIT} pet_form_replace_result_t;
/* Validates complete UTF-8, excluding C0/C1 controls, overlong sequences,
 * surrogates and out-of-range scalars. Does not normalize or truncate. */
bool pet_form_utf8(const char *text,unsigned capacity,unsigned *length,unsigned *scalars,unsigned *last);
bool pet_form2_valid(const pet_form_t *form,const pet_form_text_config_t *text);
int pet_form2_operation(const pet_form_t *form,const pet_form_text_config_t *text,const char *action);
bool pet_form2_initialize(const pet_form_t *form,const pet_form_text_config_t *text,pet_form_value_t values[PET_FORM_VARS]);
/* Cancel returns whether something changed. Invalid data stays unchanged. */
bool pet_form2_cancel(const pet_form_t *form,const pet_form_text_config_t *text,pet_form_value_t values[PET_FORM_VARS]);
bool pet_form2_allowed(const pet_form_t *form,const pet_form_text_config_t *text,const pet_form_value_t values[PET_FORM_VARS],int page,int operation);
bool pet_form2_apply(const pet_form_t *form,const pet_form_text_config_t *text,pet_form_value_t values[PET_FORM_VARS],int page,int operation);
bool pet_form2_text(const pet_form_t *form,const pet_form_text_config_t *text,const pet_form_value_t values[PET_FORM_VARS],int field,char output[121]);
/* Draft-only replacement for a trusted text provider. Caller separately owns
 * focus/lifecycle authority; this is not a wire grant or an implicit accept. */
pet_form_replace_result_t pet_form2_replace(const pet_form_t *form,const pet_form_text_config_t *text,
  pet_form_value_t values[PET_FORM_VARS],int page,int field,const char *replacement);
