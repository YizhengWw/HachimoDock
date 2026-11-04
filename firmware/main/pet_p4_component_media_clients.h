/* Track C owner adapters. Call only under matching capability compile flags.
 * Runtime/media generation must be captured together under the model snapshot
 * lock; tag an old snapshot with its old generation, never with a later lookup.
 * No implicit device admission, execution permission or clock calibration. */
#pragma once
#include "pet_p4_component_media.h"
/* Caller serializes runtime publication and snapshot capture; this checks the
 * supplied generation again before it acquires/passes any sample lease. */
void miniapp_ext_audio_apply_snapshot(const pet_cext_audio_t *audio, const pet_seq_program_t *program, uint32_t generation);
/* Success consumes/zeros lease; failure leaves ownership with caller. Must hold
 * the component snapshot/publication lock. No store mutex is taken in audio. */
bool pet_p4_audio_cext_sample_lease(pet_cmedia_sample_lease_t *lease);
/* Render thread only. Set once per immutable view snapshot, including no-clip
 * frames. Retire on hidden/exit/error; false means worker cleanup still pending.
 * Continue polling on subsequent frames. Never release the clip pin yourself. */
void renderer_ext_media_snapshot(uint32_t generation);
bool renderer_ext_clip_retire(void);
/* Explicit user cancellation; checkpoint the affected private save afterwards.
 * No boot-time dispatch or execution is granted by this API. */
bool pet_p4_miniapp_alarm_cancel(const char *widget_id, const char *alarm_id);
