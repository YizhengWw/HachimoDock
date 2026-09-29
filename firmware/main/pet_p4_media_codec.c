#include "pet_p4_media_codec.h"
static const int step_table[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int index_table[8] = {-1,-1,-1,-1,2,4,6,8};
bool pet_p4_media_decode(const uint8_t *d, size_t len, uint8_t *out, size_t cap, size_t *written) {
  if (written) *written = 0;
  if (!d || !out || !written || len < 6 || d[2] > 88 || d[3]) return false;
  size_t count = (size_t)d[4] | ((size_t)d[5] << 8);
  if (!count || count > 3840 || count * 2 > cap || len != 6 + count / 2) return false;
  if (count % 2 == 0 && (d[len-1] & 0xf0)) return false;
  int predictor = (int16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
  int index = d[2]; out[0] = d[0]; out[1] = d[1];
  for (size_t i=1; i<count; i++) {
    int code = (d[6+(i-1)/2] >> (((i-1)%2)*4)) & 15;
    int step = step_table[index], delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor += (code & 8) ? -delta : delta;
    if (predictor > 32767) predictor=32767;
    if (predictor < -32768) predictor=-32768;
    index += index_table[code & 7];
    if (index<0) index=0;
    if (index>88) index=88;
    out[i*2]=(uint8_t)predictor; out[i*2+1]=(uint8_t)(predictor>>8);
  }
  *written=count*2; return true;
}
