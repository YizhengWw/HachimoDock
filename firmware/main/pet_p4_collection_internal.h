/* Trusted owned-model implementation detail, never a package/JSON entry.
 * Only call after complete pet_collection_project validation, with immutable
 * owned rows/strings for the entire lifetime. No source may alias state/out/work.
 * Bypasses repeated record/duplicate-key validation, NOT argument/state checks.
 * The native collection host prepares/copies before publication; old public
 * projections and WASM authoring still use the fully validating entry.
 */
#pragma once
#include "pet_p4_collection.h"
pet_collection_result_t pet_collection_project_validated(const pet_collection_row_t *rows,unsigned count,
  unsigned visible,pet_collection_operation_t operation,pet_collection_state_t *state,
  pet_collection_view_t *out,pet_collection_workspace_t *workspace);
