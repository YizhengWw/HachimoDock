#include "pet_p4_cells.h"

static unsigned cell_count(const pet_cells_t *c) { return (unsigned)c->cols * c->rows; }
bool pet_cells_init(pet_cells_t *c, unsigned cols, unsigned rows, unsigned bits) {
  if (!c) return false;
  for (unsigned i = 0; i < PET_CELLS_MAX; i++) c->values[i] = 0;
  c->enabled = false; c->cols = c->rows = c->bits = 0;
  if (cols < 1 || cols > PET_CELLS_MAX_SIDE || rows < 1 || rows > PET_CELLS_MAX_SIDE
      || cols * rows > PET_CELLS_MAX || (bits != 2 && bits != 4)) return false;
  c->cols = (uint8_t)cols; c->rows = (uint8_t)rows; c->bits = (uint8_t)bits; c->enabled = true;
  return true;
}
static int hex_digit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
bool pet_cells_init_row(pet_cells_t *c, unsigned y, const char *row) {
  if (!c || !c->enabled || !row || y >= c->rows) return false;
  int32_t max = pet_cells_max_value(c);
  for (unsigned x = 0; x < c->cols; x++) {
    int digit = hex_digit(row[x]);
    if (digit < 0 || digit > max) return false;
  }
  if (row[c->cols]) return false;
  for (unsigned x = 0; x < c->cols; x++) c->values[y * c->cols + x] = (uint8_t)hex_digit(row[x]);
  return true;
}
int32_t pet_cells_max_value(const pet_cells_t *c) { return c && c->enabled ? (int32_t)((1U << c->bits) - 1U) : 0; }
int32_t pet_cells_clamp(const pet_cells_t *c, int32_t value) {
  int32_t max = pet_cells_max_value(c);
  return value < 0 ? 0 : value > max ? max : value;
}
bool pet_cells_in_range(const pet_cells_t *c, int32_t x, int32_t y) {
  return c && c->enabled && x >= 0 && y >= 0 && x < c->cols && y < c->rows;
}
int32_t pet_cells_get(const pet_cells_t *c, int32_t x, int32_t y) {
  return pet_cells_in_range(c, x, y) ? c->values[(unsigned)y * c->cols + (unsigned)x] : -1;
}
bool pet_cells_set(pet_cells_t *c, int32_t x, int32_t y, int32_t value) {
  if (!pet_cells_in_range(c, x, y)) return false;
  uint8_t next = (uint8_t)pet_cells_clamp(c, value), *slot = &c->values[(unsigned)y * c->cols + (unsigned)x];
  if (*slot == next) return false;
  *slot = next; return true;
}
bool pet_cells_fill(pet_cells_t *c, int32_t value) {
  if (!c || !c->enabled) return false;
  uint8_t next = (uint8_t)pet_cells_clamp(c, value); bool changed = false;
  for (unsigned i = 0; i < cell_count(c); i++) { if (c->values[i] != next) changed = true; c->values[i] = next; }
  return changed;
}
int32_t pet_cells_count(const pet_cells_t *c, int32_t value) {
  if (!c || !c->enabled || value < 0 || value > pet_cells_max_value(c)) return 0;
  int32_t count = 0;
  for (unsigned i = 0; i < cell_count(c); i++) if (c->values[i] == value) count++;
  return count;
}
int32_t pet_cells_run(const pet_cells_t *c, int32_t x, int32_t y, int32_t value) {
  if (pet_cells_get(c, x, y) != value || value < 0) return 0;
  static const int8_t dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
  int32_t best = 0;
  for (unsigned d = 0; d < 4; d++) {
    int32_t run = 1;
    for (int sign = -1; sign <= 1; sign += 2) {
      int32_t cx = x, cy = y;
      for (unsigned step = 0; step < PET_CELLS_MAX_SIDE; step++) {
        cx += sign * dirs[d][0]; cy += sign * dirs[d][1];
        if (pet_cells_get(c, cx, cy) != value) break;
        run++;
      }
    }
    if (run > best) best = run;
  }
  return best;
}
unsigned pet_cells_export(const pet_cells_t *c, int32_t *out, unsigned capacity) {
  if (!c || !c->enabled || !out) return 0;
  unsigned count = cell_count(c); if (count > capacity) count = capacity;
  for (unsigned i = 0; i < count; i++) out[i] = c->values[i];
  return count;
}
unsigned pet_cells_import(pet_cells_t *c, const int32_t *in, unsigned count, bool *changed) {
  if (changed) *changed = false;
  if (!c || !c->enabled || !in) return 0;
  if (count > cell_count(c)) count = cell_count(c);
  for (unsigned i = 0; i < count; i++) {
    uint8_t next = (uint8_t)pet_cells_clamp(c, in[i]);
    if (c->values[i] != next) { c->values[i] = next; if (changed) *changed = true; }
  }
  return count;
}
