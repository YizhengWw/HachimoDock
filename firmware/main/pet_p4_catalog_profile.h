/* Normal capacity stays 32. A silent acceptance-only image adds ONE slot so
 * two owned fixtures fit without removing retained hidden user data. Never
 * return to a 32-slot image until owned fixtures are removed and catalog is
 * verified within its original bound. Same format/paths/atomic publication. */
#pragma once
#include "pet_p4_component_features.h"
#ifndef PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE
#define PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE 0
#endif
#if PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE && !PET_COMPONENT_PROGRAM_PARENT_UNIFIED
#error "Private acceptance capacity requires the complete unified parent"
#endif
#if PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE
#define PET_P4_MINIAPP_CATALOG_MAX 33
#else
#define PET_P4_MINIAPP_CATALOG_MAX 32
#endif
