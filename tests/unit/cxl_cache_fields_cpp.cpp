/* SPDX-License-Identifier: Apache-2.0 */

/* Compile the same layout probe as C++ so excl/types.h selects its
 * qualifier-free C++/nvcc view of the cache. */
#include "cxl_cache_fields.c"
