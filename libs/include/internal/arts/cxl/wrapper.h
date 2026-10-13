/******************************************************************************
 * Copyright 2019 Battelle Memorial Institute
 * Licensed under the Apache License, Version 2.0
 ******************************************************************************/
#ifndef ARTS_CXL_WRAPPER_H
#define ARTS_CXL_WRAPPER_H
#ifdef __cplusplus
extern "C" {
#endif

#define CACHELINE_SIZE 64

/** Round @p x up to the nearest multiple of @p a (power of two). */
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* The device library's interface, from whichever library the build resolved
 * (the vendored fake library or the device SDK); only a translation unit of
 * a CXL build includes this header. */
#include <MemOps.h>
#include <SharedAlloc.h>
#define SHARED_MALLOC(...) SHARED_CXL_MALLOC(__VA_ARGS__)
#define GLOBAL_MALLOC(...) GLOBAL_CXL_MALLOC(__VA_ARGS__)
#define GLOBAL_MALLOC_DEV(...) GLOBAL_CXL_MALLOC_DEV(__VA_ARGS__)
#define GLOBAL_FREE(...) GLOBAL_CXL_FREE(__VA_ARGS__)
#define SHARED_FREE(...) SHARED_CXL_FREE(__VA_ARGS__)
#define SHARED_MALLOC_INITIALIZED(...)                                         \
  SHARED_CXL_MALLOC_INITIALIZED(__VA_ARGS__)
#define LAST_SHARED_MALLOC(...) LAST_SHARED_CXL_MALLOC(__VA_ARGS__)
#define IS_CXL_PTR(...) IS_FAM_PTR(__VA_ARGS__)
#define GET_CXL_DEV_ID(...) GET_FAM_DEV_ID(__VA_ARGS__)
#define GET_CXL_REGION_DEV_ID(...) GET_FAM_REGION_DEV_ID(__VA_ARGS__)
#define GET_CXL_DEV_COUNT(...) GET_FAM_DEV_COUNT(__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* ARTS_CXL_WRAPPER_H */
