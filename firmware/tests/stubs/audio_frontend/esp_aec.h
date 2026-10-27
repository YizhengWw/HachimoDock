#pragma once
#include <stdint.h>
enum { AEC_MODE_VOIP_LOW_COST, AEC_MODE_FD_LOW_COST, AEC_MODE_FD_HIGH_PERF };
enum { AEC_NLP_LEVEL_AGGR, AEC_NLP_LEVEL_VERYAGGR };
typedef struct {
  int mic_num, ref_num, out_num, filter_length, sample_rate;
  unsigned caps;
  int mode, nlp_level;
} aec_config_t;
typedef struct { aec_config_t config; } aec_handle_t;
aec_handle_t *aec_create_from_config(aec_config_t *config);
void aec_destroy(aec_handle_t *handle);
int aec_get_chunksize(aec_handle_t *handle);
void aec_process(const aec_handle_t *handle, int16_t *mic, int16_t *reference, int16_t *output);
