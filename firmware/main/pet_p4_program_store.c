#include "pet_p4_program_store.h"
#include <limits.h>

/* A future runtime budget/layout change must introduce a new explicit wire
 * format; it cannot silently resize existing persisted snapshots. */
_Static_assert(PET_PROGRAM_CELLS >= PET_PROGRAM_STORE_CELLS && PET_PROGRAM_STORE_CELLS == 64U, "P4S1 v1 persists exactly 64 cells");
_Static_assert(PET_PROGRAM_OUTPUTS == 8U, "P4S1 v1 requires 8 outputs");
_Static_assert(PET_PROGRAM_STORE_BYTES == 320U, "P4S1 v1 wire size");
_Static_assert(PET_PROGRAM_STORE_TEXT_CAPACITY == 652U, "P4S1 text size");

static bool span_valid(const void *p, size_t n) {
  return p && n && (uintptr_t)p <= UINTPTR_MAX - (n - 1U);
}
static bool overlap(const void *a, size_t an, const void *b, size_t bn) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  return x <= y ? y - x < an : x - y < bn;
}
static bool layout_valid(uint32_t schema, unsigned cells) {
  return schema != 0 && cells <= PET_PROGRAM_CELLS;
}
static bool snapshot_valid(const pet_program_snapshot_t *s) {
  if (!layout_valid(s->schema, s->cells)) return false;
  for (unsigned i = s->cells; i < PET_PROGRAM_CELLS; i++) if (s->values[i]) return false;
  return true;
}
static void zero_bytes(void *p, size_t n) {
  uint8_t *out = p;
  for (size_t i = 0; i < n; i++) out[i] = 0;
}
static int32_t signed32(uint32_t v) {
  return v <= INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX - v);
}
static void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8U * i));
}
static uint32_t checksum(const uint8_t *bytes, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; i++) {
    if (i >= 28U && i < 32U) continue;
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
  }
  return ~crc;
}
size_t pet_program_store_size(const pet_program_snapshot_t *s) {
  if (!span_valid(s, sizeof(*s)) || !snapshot_valid(s)) return 0;
  return s->cells > PET_PROGRAM_STORE_CELLS ? PET_PROGRAM_STORE_V2_BYTES : PET_PROGRAM_STORE_BYTES;
}
pet_program_store_result_t pet_program_store_encode(uint8_t *out, size_t capacity,
    const pet_program_snapshot_t *s) {
  if (!span_valid(s, sizeof(*s))) return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  size_t size = PET_P4_PROGRAM_V2 && s->cells > PET_PROGRAM_STORE_CELLS ? PET_PROGRAM_STORE_V2_BYTES : PET_PROGRAM_STORE_BYTES;
  if (!span_valid(out, size) || overlap(out, size, s, sizeof(*s))) return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  if (!snapshot_valid(s)) return PET_PROGRAM_STORE_BAD_STATE;
  if (capacity < size) return PET_PROGRAM_STORE_BAD_LENGTH;
  bool v2 = size == PET_PROGRAM_STORE_V2_BYTES;
  unsigned slots = v2 ? PET_PROGRAM_STORE_V2_CELLS : PET_PROGRAM_STORE_CELLS;
  zero_bytes(out, size);
  out[0]='P';out[1]='4';out[2]='S';out[3]=v2?'2':'1';out[4]=v2?2:1;
  out[6]=PET_PROGRAM_STORE_HEADER;put32(out+8,(uint32_t)size);
  put32(out+12,s->schema);put32(out+16,s->revision);
  out[20]=(uint8_t)s->cells;
  if(v2){out[21]=(uint8_t)(s->cells>>8);out[22]=PET_PROGRAM_OUTPUTS;out[23]=4;}
  else{out[21]=PET_PROGRAM_OUTPUTS;out[22]=4;}
  for(unsigned i=0;i<s->cells;i++)put32(out+PET_PROGRAM_STORE_HEADER+i*4U,(uint32_t)s->values[i]);
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)put32(out+PET_PROGRAM_STORE_HEADER+(slots+i)*4U,(uint32_t)s->outputs[i]);
  put32(out+28,checksum(out,size));return PET_PROGRAM_STORE_OK;
}
static int hex_digit(char c) {
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  return -1;
}
typedef struct {const uint8_t *data;bool hex;} store_reader_t;
static uint8_t read_byte(store_reader_t r,size_t i){
  return r.hex?(uint8_t)((unsigned)hex_digit((char)r.data[9U+2U*i])*16U+(unsigned)hex_digit((char)r.data[10U+2U*i])):r.data[i];
}
static uint16_t read16(store_reader_t r,size_t i){return (uint16_t)(read_byte(r,i)|((uint16_t)read_byte(r,i+1U)<<8));}
static uint32_t read32(store_reader_t r,size_t i){return (uint32_t)read_byte(r,i)|((uint32_t)read_byte(r,i+1U)<<8)|((uint32_t)read_byte(r,i+2U)<<16)|((uint32_t)read_byte(r,i+3U)<<24);}
static uint32_t reader_checksum(store_reader_t r,size_t length){
  uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<length;i++){if(i>=28U&&i<32U)continue;crc^=read_byte(r,i);for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1U)?UINT32_C(0xedb88320):0U);}
  return ~crc;
}
static pet_program_store_result_t decode_value(pet_program_snapshot_t *out,uint32_t expected_schema,
    pet_program_cells_t expected_cells,store_reader_t reader,size_t length){
  bool v2=length==PET_PROGRAM_STORE_V2_BYTES;
  if(read_byte(reader,0)!='P'||read_byte(reader,1)!='4'||read_byte(reader,2)!='S'||read_byte(reader,3)!=(v2?'2':'1')||read_byte(reader,4)!=(v2?2:1)||read_byte(reader,5)
      ||read16(reader,6)!=PET_PROGRAM_STORE_HEADER||read32(reader,8)!=length||read32(reader,24)
      ||(v2?(read_byte(reader,22)!=PET_PROGRAM_OUTPUTS||read_byte(reader,23)!=4):(read_byte(reader,21)!=PET_PROGRAM_OUTPUTS||read_byte(reader,22)!=4||read_byte(reader,23))))return PET_PROGRAM_STORE_BAD_HEADER;
  if(read32(reader,28)!=reader_checksum(reader,length))return PET_PROGRAM_STORE_BAD_CHECKSUM;
  unsigned cells=v2?read16(reader,20):read_byte(reader,20),slots=v2?PET_PROGRAM_STORE_V2_CELLS:PET_PROGRAM_STORE_CELLS;
  if(!layout_valid(read32(reader,12),cells)||cells>slots||(v2&&cells<=PET_PROGRAM_STORE_CELLS))return PET_PROGRAM_STORE_BAD_STATE;
  if(read32(reader,12)!=expected_schema||cells!=expected_cells)return PET_PROGRAM_STORE_LAYOUT_MISMATCH;
  for(unsigned i=cells;i<slots;i++)if(read32(reader,PET_PROGRAM_STORE_HEADER+i*4U))return PET_PROGRAM_STORE_BAD_STATE;
  /* Publication is all-or-nothing: all lengths, checksum and reserved cells
   * have been validated before the first output write. */
  zero_bytes(out,sizeof(*out));out->schema=expected_schema;out->cells=expected_cells;out->revision=read32(reader,16);
  for(unsigned i=0;i<cells;i++)out->values[i]=signed32(read32(reader,PET_PROGRAM_STORE_HEADER+i*4U));
  for(unsigned i=0;i<PET_PROGRAM_OUTPUTS;i++)out->outputs[i]=signed32(read32(reader,PET_PROGRAM_STORE_HEADER+(slots+i)*4U));
  return PET_PROGRAM_STORE_OK;
}
pet_program_store_result_t pet_program_store_decode(pet_program_snapshot_t *out,
    uint32_t expected_schema,pet_program_cells_t expected_cells,const uint8_t *bytes,size_t length){
  if(!layout_valid(expected_schema,expected_cells)||!out||!bytes)return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  if(length!=PET_PROGRAM_STORE_BYTES&&!(PET_P4_PROGRAM_V2&&length==PET_PROGRAM_STORE_V2_BYTES))return PET_PROGRAM_STORE_BAD_LENGTH;
  if(!span_valid(out,sizeof(*out))||!span_valid(bytes,length)||overlap(out,sizeof(*out),bytes,length))return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  return decode_value(out,expected_schema,expected_cells,(store_reader_t){bytes,false},length);
}
pet_program_store_result_t pet_program_store_encode_text(char *out, size_t capacity,
    const pet_program_snapshot_t *s) {
  if(!span_valid(s,sizeof(*s)))return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  size_t size=PET_P4_PROGRAM_V2 && s->cells>PET_PROGRAM_STORE_CELLS?PET_PROGRAM_STORE_V2_BYTES:PET_PROGRAM_STORE_BYTES;
  size_t chars=11U+size*2U;
  if(!span_valid(out,chars+1U)||overlap(out,chars+1U,s,sizeof(*s)))return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  if(capacity<chars+1U)return PET_PROGRAM_STORE_BAD_LENGTH;
  /* Binary encode is transactional. Expand backwards in the caller's output
   * only after validation; every hex destination is above unread source bytes.
   * No format-sized stack buffer and no fallible operation after first store. */
  pet_program_store_result_t result=pet_program_store_encode((uint8_t*)out,capacity,s);
  if(result!=PET_PROGRAM_STORE_OK)return result;
  static const char prefix[]="{\"p4s1\":\"",digits[]="0123456789abcdef";
  for(size_t i=size;i>0;i--){uint8_t byte=(uint8_t)out[i-1U];out[9U+(i-1U)*2U]=digits[byte>>4];out[10U+(i-1U)*2U]=digits[byte&15U];}
  for(size_t i=0;i<sizeof(prefix)-1U;i++)out[i]=prefix[i];
  out[5]=size==PET_PROGRAM_STORE_V2_BYTES?'2':'1';
  out[chars-2U]='"';out[chars-1U]='}';out[chars]=0;return PET_PROGRAM_STORE_OK;
}
pet_program_store_result_t pet_program_store_decode_text(pet_program_snapshot_t *out,
    uint32_t expected_schema, pet_program_cells_t expected_cells, const char *text, size_t length) {
  if (!layout_valid(expected_schema, expected_cells) || !out || !text)
    return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  bool v2=PET_P4_PROGRAM_V2 && length==11U+PET_PROGRAM_STORE_V2_BYTES*2U;
  if (length != PET_PROGRAM_STORE_TEXT_LENGTH && !v2) return PET_PROGRAM_STORE_BAD_LENGTH;
  size_t size=v2?PET_PROGRAM_STORE_V2_BYTES:PET_PROGRAM_STORE_BYTES;
  if (!span_valid(out, sizeof(*out)) || !span_valid(text, length)
      || overlap(out, sizeof(*out), text, length)) return PET_PROGRAM_STORE_INVALID_ARGUMENT;
  static const char prefix[] = "{\"p4s1\":\"";
  for (size_t i = 0; i < sizeof(prefix) - 1U; i++)
    if (text[i] != (i==5U && v2 ? '2' : prefix[i])) return PET_PROGRAM_STORE_BAD_ENCODING;
  if (text[length - 2U] != '"' || text[length - 1U] != '}') return PET_PROGRAM_STORE_BAD_ENCODING;
  for(size_t i=9U;i<length-2U;i++)if(hex_digit(text[i])<0)return PET_PROGRAM_STORE_BAD_ENCODING;
  return decode_value(out,expected_schema,expected_cells,(store_reader_t){(const uint8_t*)text,true},size);
}
