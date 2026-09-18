// SPDX-License-Identifier: GPL-3.0-only

#ifndef HLSL_STORAGE_PLAN_H
#define HLSL_STORAGE_PLAN_H

#include <stdbool.h>

typedef enum HLSLTempStorage {
    HLSL_TEMP_STORAGE_FLOAT = 0,
    HLSL_TEMP_STORAGE_RAW_UINT
} HLSLTempStorage;

typedef struct HLSLStoragePlan {
    unsigned char *register_storage;
    int register_count;
    HLSLTempStorage default_storage;
} HLSLStoragePlan;

#endif
