/// @file fam_alloc_dist.c
/// @brief Every rank and every worker allocating from the real pool at once.
///
/// Fails on: a block outside the allocating rank's slice, an unaligned block,
/// or one block handed to two workers -- the defects a single-threaded or
/// single-rank test cannot see.

#include "arts.h"
#include "arts/fam/pool.h"
#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHURNERS 8u
#define ROUNDS 2000
#define LIVE 12
#define MAX_BYTES 4096u

static void churn_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  if (paramc < 1) {
    arts_printf("FAIL fam_alloc_dist: no id\n");
    arts_test_fail();
    return;
  }
  unsigned char id = (unsigned char)(1u + (unsigned)paramv[0]);
  unsigned seed = (unsigned)paramv[0] * 7919u + 13u;
  void *live[LIVE];
  size_t len[LIVE];
  memset(live, 0, sizeof(live));
  memset(len, 0, sizeof(len));
  for (int r = 0; r < ROUNDS; r++) {
    int i = rand_r(&seed) % LIVE;
    if (live[i]) {
      const unsigned char *b = (const unsigned char *)live[i];
      for (size_t k = 0; k < len[i]; k++) {
        if (b[k] != id) {
          arts_printf("FAIL fam_alloc_dist: block of %u held %u at byte %zu\n",
                      (unsigned)id, (unsigned)b[k], k);
          arts_test_fail();
          break;
        }
      }
      arts_fam_free(live[i]);
      live[i] = NULL;
    } else {
      size_t n = (size_t)(rand_r(&seed) % MAX_BYTES) + 1u;
      void *p = arts_fam_alloc(n);
      if (((uintptr_t)p % ARTS_FAM_GRANULE) != 0 || !arts_fam_contains(p)) {
        arts_printf("FAIL fam_alloc_dist: %p is unaligned or outside the "
                    "pool\n",
                    p);
        arts_test_fail();
        return;
      }
      memset(p, (int)id, n);
      live[i] = p;
      len[i] = n;
    }
  }
  for (int i = 0; i < LIVE; i++) {
    if (live[i]) {
      arts_fam_free(live[i]);
    }
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS fam_alloc_dist: %u churners per rank x %d rounds\n",
              CHURNERS, ROUNDS);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned n = arts_get_total_ranks();
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t done =
      arts_edt_create(done_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(fe, done, 0, DB_MODE_NULL);
  for (unsigned r = 0; r < n; r++) {
    for (unsigned c = 0; c < CHURNERS; c++) {
      uint64_t id = (uint64_t)(r * CHURNERS + c);
      (void)arts_edt_create(
          churn_edt, 1, &id, 0,
          &(arts_edt_hint_t){.rank = r, .finish_event = fe});
    }
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
