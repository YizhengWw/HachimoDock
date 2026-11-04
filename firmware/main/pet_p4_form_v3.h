/* p4-app-form-v3: bounded multi-field text forms with an on-device keyboard.
 * Product code shared verbatim by the device and the PC preview core; never a
 * package executable. Up to four UTF-8 text fields (<=63 bytes each), one
 * keyboard mode per field (numeric keypad, Latin T9 groups, pinyin T9 over
 * the bounded generated table) and the same eight logical actions as v2:
 * previous/next (field while closed, letter/candidate cursor while editing),
 * decrease/increase (key, group or digit), accept (open, commit candidate,
 * or atomically commit all drafts), cancel (clear pending digits or discard),
 * append (append key/letter or push a pinyin digit) and erase. No heap/IO. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_form_v2.h"
#ifndef PET_FORM_V3_ENABLED
#define PET_FORM_V3_ENABLED 0
#endif
#define PET_FORM3_ENGINE "p4-app-form-v3"
#define PET_FORM3_FIELDS 4
#define PET_FORM3_VARS 32
#define PET_FORM3_ACTIONS 8
#define PET_FORM3_BUFFER_DIGITS 6
#define PET_FORM3_CANDIDATES_MAX 64
#define PET_FORM3_PAGE 5
#define PET_FORM3_NUMERIC_KEYS 12
#define PET_FORM3_GROUPS 10
enum {PET_FORM3_NUMERIC=0,PET_FORM3_LATIN=1,PET_FORM3_PINYIN=2};
typedef struct {char id[16];uint8_t var,draft_var,mode,max_bytes;} pet_form3_field_t;
typedef struct {
  bool enabled;
  uint8_t count,page,focus_var,active_var,key_var,cursor_var,buffer_var,label_var,candidates_var;
  pet_form3_field_t fields[PET_FORM3_FIELDS];
  char actions[PET_FORM3_ACTIONS][48];
} pet_form3_t;
typedef struct {int32_t number;char text[64];} pet_form3_value_t;
extern const char pet_form3_numeric_keys[PET_FORM3_NUMERIC_KEYS+1];
extern const char *const pet_form3_groups[PET_FORM3_GROUPS];
bool pet_form3_valid(const pet_form3_t *form);
int pet_form3_operation(const pet_form3_t *form,const char *action);
int pet_form3_mode(const char *name);
int pet_form3_field_index(const pet_form3_t *form,const char *id);
/* Pinyin candidates for a T9 digit buffer: exact-code syllables first, then
 * longer syllables sharing the prefix, dictionary order, deduplicated. */
unsigned pet_form3_candidates(const char *digits,uint16_t *out,unsigned max);
/* Exact-code syllables joined by '|' (empty when none). */
void pet_form3_syllables(const char *digits,char *out,unsigned capacity);
bool pet_form3_initialize(const pet_form3_t *form,pet_form3_value_t values[PET_FORM3_VARS]);
/* Full discard (hide/leave): drafts, keyboard state and editing flag. */
bool pet_form3_cancel(const pet_form3_t *form,pet_form3_value_t values[PET_FORM3_VARS]);
bool pet_form3_allowed(const pet_form3_t *form,const pet_form3_value_t values[PET_FORM3_VARS],int page,int operation);
bool pet_form3_apply(const pet_form3_t *form,pet_form3_value_t values[PET_FORM3_VARS],int page,int operation);
bool pet_form3_text(const pet_form3_t *form,const pet_form3_value_t values[PET_FORM3_VARS],int field,char out[121]);
pet_form_replace_result_t pet_form3_replace(const pet_form3_t *form,pet_form3_value_t values[PET_FORM3_VARS],int page,int field,const char *replacement);
