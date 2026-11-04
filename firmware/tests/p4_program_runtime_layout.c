/* Object symbol sizes provide target ABI layout without executing P4 code. */
#include "pet_p4_program_runtime.h"
char pet_runtime_layout_image[sizeof(pet_program_image_t)];
char pet_runtime_layout_runtime[sizeof(pet_program_runtime_t)];
char pet_runtime_layout_snapshot[sizeof(pet_program_snapshot_t)];
char pet_runtime_layout_report[sizeof(pet_program_report_t)];
