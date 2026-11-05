/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/system/shutdown.h"

#if defined(ARTS_USE_CXL) && !defined(ARTS_CXL_REAL)
#include <dlfcn.h>
#include <link.h>
#include <stddef.h>
#include <stdint.h>

#include "arts/system/print.h"

/* The loaded object whose image holds `addr`, and its dynamic section. */
struct arts_fini_object_s {
  uintptr_t addr;
  ElfW(Addr) base;
  const ElfW(Dyn) * dynamic;
};

static int arts_fini_object_find(struct dl_phdr_info *info, size_t size,
                                 void *data) {
  (void)size;
  struct arts_fini_object_s *object = data;
  const ElfW(Dyn) *dynamic = NULL;
  bool holds = false;
  for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    uintptr_t start = (uintptr_t)(info->dlpi_addr + ph->p_vaddr);
    if (ph->p_type == PT_LOAD && object->addr >= start &&
        object->addr - start < ph->p_memsz) {
      holds = true;
    } else if (ph->p_type == PT_DYNAMIC) {
      dynamic = (const ElfW(Dyn) *)start;
    }
  }
  if (!holds) {
    return 0;
  }
  object->base = info->dlpi_addr;
  object->dynamic = dynamic;
  return 1;
}

/* The fake library's exit work (its region's unlink in the process that
 * created the region, and its flush trace) runs from its own finalizers,
 * which an exit by exit_group never runs; this runs them in the order the
 * dynamic loader's exit does: the finalizer array from its last entry to its
 * first, then the finalizer function.  The library is found through a
 * function of its own, looked up past the calling object so that an
 * executable's canonical entry for the function cannot stand in for it. */
void arts_fake_cxl_fini_replay(void) {
  static _Atomic(bool) replayed = false;
  if (atomic_exchange_explicit(&replayed, true, memory_order_relaxed)) {
    return;
  }
  void *fn = dlsym(RTLD_NEXT, "fake_cxl_shared_malloc");
  struct arts_fini_object_s object = {.addr = (uintptr_t)fn};
  if (fn == NULL || dl_iterate_phdr(arts_fini_object_find, &object) == 0 ||
      object.dynamic == NULL) {
    arts_print_line("[ARTS] the fake CXL library's finalizers were not found: "
                    "its exit work is not replayed\n");
    return;
  }
  ElfW(Addr) fini_array = 0;
  size_t fini_array_bytes = 0;
  ElfW(Addr) fini = 0;
  for (const ElfW(Dyn) *d = object.dynamic; d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_FINI_ARRAY) {
      fini_array = d->d_un.d_ptr;
    } else if (d->d_tag == DT_FINI_ARRAYSZ) {
      fini_array_bytes = (size_t)d->d_un.d_val;
    } else if (d->d_tag == DT_FINI) {
      fini = d->d_un.d_ptr;
    }
  }
  if (fini_array != 0) {
    void (*const *entries)(void) =
        (void (*const *)(void))(object.base + fini_array);
    for (size_t i = fini_array_bytes / sizeof(*entries); i-- > 0;) {
      entries[i]();
    }
  }
  if (fini != 0) {
    ((void (*)(void))(object.base + fini))();
  }
}
#else
void arts_fake_cxl_fini_replay(void) {}
#endif
