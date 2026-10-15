/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_install_signal_handlers must load the unwinder (libgcc_s) before it
 * installs any handler: backtrace() dlopens it on first use, and the crash and
 * term handlers call backtrace() from signal context, where dlopen/malloc are
 * not async-signal-safe.  The test #includes signals.c (standalone stubs) and
 * checks with dl_iterate_phdr that libgcc_s is absent before the call and
 * present after it.  Exit status carries the verdict.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <link.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
void arts_enter_shutdown_state(bool initiator) { (void)initiator; }
_Noreturn void arts_exit_now(uint8_t status) { _exit(status); }

#include "../../libs/src/core/system/signals.c"

ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

static int match_unwinder(struct dl_phdr_info *info, size_t size, void *arg) {
  (void)size;
  if (info->dlpi_name != NULL && strstr(info->dlpi_name, "libgcc_s") != NULL) {
    *(bool *)arg = true;
    return 1;
  }
  return 0;
}

static bool unwinder_loaded(void) {
  bool found = false;
  dl_iterate_phdr(match_unwinder, &found);
  return found;
}

int main(void) {
  if (unwinder_loaded()) {
    printf("FAIL signals_unwinder_preload: libgcc_s already loaded before "
           "install\n");
    return 1;
  }
  arts_install_signal_handlers();
  if (!unwinder_loaded()) {
    printf("FAIL signals_unwinder_preload: libgcc_s not loaded after "
           "install\n");
    return 1;
  }
  printf("PASS signals_unwinder_preload\n");
  return 0;
}
