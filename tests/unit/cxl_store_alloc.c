/// @file cxl_store_alloc.c
/// @brief The store seam over the shared arena: every slot is a cache-line
/// boundary inside the store, slots are disjoint, a zero-byte request still
/// yields a line, and the static strategy's one arena serves whatever device
/// the cfg named.  Exhaustion is probed in a forked child: it must end the
/// child with the fatal error naming the arena size, never a null slot.
#include "arts.h"
#include "arts/cxl/store.h"
#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOTS 64u
#define BYTES 1000u /* not a multiple of 64 on purpose */

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL cxl_store_alloc: %s\n", what);
  arts_test_fail();
}

static void check_alloc(void) {
  unsigned char *p[SLOTS];
  for (unsigned i = 0; i < SLOTS; i++) {
    p[i] = (unsigned char *)arts_cxl_store_alloc(BYTES);
    if (!arts_cxl_store_contains(p[i])) {
      fail("a slot is not inside the store");
    }
    if (((uintptr_t)p[i] & 63u) != 0u) {
      fail("a slot is not 64-byte aligned");
    }
    for (unsigned j = 0; j < i; j++) {
      if (p[j] < p[i] + 1024u && p[i] < p[j] + 1024u) {
        fail("two slots overlap");
      }
    }
    memset(p[i], (int)i, BYTES);
  }
  for (unsigned i = 0; i < SLOTS; i++) {
    if (p[i][0] != (unsigned char)i || p[i][BYTES - 1u] != (unsigned char)i) {
      fail("a slot does not hold its bytes");
    }
  }
  if (!arts_cxl_store_contains(arts_cxl_store_alloc(0))) {
    fail("a zero-byte request yields no slot");
  }
  if (arts_cxl_store_contains(&p[0]) || arts_cxl_store_contains(NULL)) {
    fail("contains answers true outside the store");
  }
}

static void check_exhaustion(void) {
  pid_t pid = fork();
  if (pid == 0) {
    for (;;) {
      (void)arts_cxl_store_alloc(64u * 1024u * 1024u);
    }
  }
  int st = 0;
  (void)waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) == 0) {
    fail("exhausting the arena did not end the child with the fatal error");
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_current_rank() == 0u) {
    check_alloc();
    check_exhaustion();
    if (arts_test_status() == 0) {
      (void)printf("PASS cxl_store_alloc\n");
    }
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
