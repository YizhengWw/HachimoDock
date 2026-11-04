/* Bounded keyed collection model, NOT an installed component capability.
 * Trusted parent owns immutable rows/strings and serializes dataset/state and
 * lifecycle for the entire call. Native pointers never come from a package.
 * No allocation, sorting, IO, driver, clock, callbacks or full record copies.
 * The parent retains complete records; display truncation never edits them.
 * Projection is not authority for later input: the parent must bind dataset
 * identity/revision/current UI lease and reject stale row gestures separately.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PET_COLLECTION_ROWS 128U
#define PET_COLLECTION_VISIBLE 8U
#define PET_COLLECTION_TEXT 64U
#define PET_COLLECTION_SOURCE_TEXT 512U
typedef struct {const char *key,*label,*detail;} pet_collection_row_t;
/* Shared scalar/control/byte rule for bounded visible transport text. Native
 * caller supplies a readable terminated string; no normalization or truncation. */
bool pet_collection_text_valid(const char *text,size_t maximum_bytes,bool nonempty);
typedef struct {uint16_t offset;char selected[PET_COLLECTION_TEXT];} pet_collection_state_t;
typedef enum {
  PET_COLLECTION_STATUS,PET_COLLECTION_NEXT,PET_COLLECTION_PREVIOUS,
  PET_COLLECTION_UP,PET_COLLECTION_DOWN,PET_COLLECTION_CLEAR,
  PET_COLLECTION_ROW_0,PET_COLLECTION_ROW_1,PET_COLLECTION_ROW_2,PET_COLLECTION_ROW_3,
  PET_COLLECTION_ROW_4,PET_COLLECTION_ROW_5,PET_COLLECTION_ROW_6,PET_COLLECTION_ROW_7
} pet_collection_operation_t;
typedef struct {
  char key[PET_COLLECTION_TEXT],label[PET_COLLECTION_TEXT],detail[PET_COLLECTION_TEXT];
  int16_t index;
  bool present,selected,truncated;
} pet_collection_visible_t;
typedef struct {
  pet_collection_visible_t rows[PET_COLLECTION_VISIBLE];
  char selection_key[PET_COLLECTION_TEXT],selected_label[PET_COLLECTION_TEXT];
  uint16_t count,offset,page,pages;
  int16_t selected_index;
  uint8_t visible;
  bool can_previous,can_next,can_up,can_down;
} pet_collection_view_t;
/* Allocate this caller-private scratch off task stack. A failure may overwrite
 * scratch but never caller state/output. Do not move/copy live parent storage.
 * All objects and source string ranges must be nonoverlapping. */
typedef struct {pet_collection_view_t view;pet_collection_state_t state;} pet_collection_workspace_t;
typedef enum {PET_COLLECTION_OK,PET_COLLECTION_INVALID,PET_COLLECTION_BAD_DATA,
  PET_COLLECTION_BAD_STATE,PET_COLLECTION_NO_ROW} pet_collection_result_t;
/* STATUS clamps only the returned view, never saved state. Explicit actions
 * change navigation/selection, not records. Hidden selected IDs are retained.
 * Both display fields accept 0..512 valid UTF-8 bytes without control scalars;
 * unique nonempty keys accept 1..63. Visible text is cut at a UTF-8 boundary,
 * with an explicit flag, never reused as a key or written over source text.
 * A missing detail pointer is invalid; an absent field is the trusted empty
 * string. Out always has 8 zeroed slots; only `visible` slots are in the view.
 */
pet_collection_result_t pet_collection_project(const pet_collection_row_t *rows,unsigned count,
  unsigned visible,pet_collection_operation_t operation,pet_collection_state_t *state,
  pet_collection_view_t *out,pet_collection_workspace_t *workspace);
