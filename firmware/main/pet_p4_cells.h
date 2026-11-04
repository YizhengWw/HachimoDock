/* p4-cells-v1: bounded grid of 2/4-bit cells shared by the bounded runtime,
 * the PC preview (WASM) and host tests. Pure C: no RTOS, heap, JSON, IO or
 * callbacks. Every loop is bounded by the 256-cell / 16-side ceilings. The
 * owner (runtime parser) decides where the struct lives (PSRAM runtime). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PET_CELLS_ENGINE "p4-cells-v1"
#define PET_CELLS_MAX_SIDE 16U
#define PET_CELLS_MAX 256U
typedef struct {
  bool enabled;
  uint8_t cols, rows, bits; /* bits is 2 or 4 */
  uint8_t values[PET_CELLS_MAX]; /* row-major: index = y * cols + x */
} pet_cells_t;
/* Zeroes the grid; false (and disabled) for an unsupported shape. */
bool pet_cells_init(pet_cells_t *c, unsigned cols, unsigned rows, unsigned bits);
/* One init row: exactly cols lower/upper-case hex digits, each below 2^bits. */
bool pet_cells_init_row(pet_cells_t *c, unsigned y, const char *row);
int32_t pet_cells_max_value(const pet_cells_t *c);
int32_t pet_cells_clamp(const pet_cells_t *c, int32_t value);
bool pet_cells_in_range(const pet_cells_t *c, int32_t x, int32_t y);
/* Out of range reads -1 so conditions can tell the border from value 0. */
int32_t pet_cells_get(const pet_cells_t *c, int32_t x, int32_t y);
/* Clamps the value into 0..2^bits-1; out of range is a no-op returning false.
 * Returns true only when the stored value changed. */
bool pet_cells_set(pet_cells_t *c, int32_t x, int32_t y, int32_t value);
bool pet_cells_fill(pet_cells_t *c, int32_t value);
int32_t pet_cells_count(const pet_cells_t *c, int32_t value);
/* Longest straight run of `value` through (x, y) over the four line directions
 * (horizontal, vertical, both diagonals); 0 when (x, y) is out of range or
 * does not hold `value`. A 5-in-a-row win test is run >= 5. */
int32_t pet_cells_run(const pet_cells_t *c, int32_t x, int32_t y, int32_t value);
/* Program VM mirror: export the first cols*rows cells into int32 slots and
 * import them back (clamped). Both return the number of cells touched. */
unsigned pet_cells_export(const pet_cells_t *c, int32_t *out, unsigned capacity);
unsigned pet_cells_import(pet_cells_t *c, const int32_t *in, unsigned count, bool *changed);
