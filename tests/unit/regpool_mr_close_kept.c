/* SPDX-License-Identifier: Apache-2.0
 *
 * regpool_mr_close_kept — a registration whose close the provider refuses is
 * kept, never freed under: the teardown of the registered pool and the
 * libfabric transport (memory/regpool.c, transport/net.c) when one slab's
 * fi_close(mr) fails.
 *
 * The fabric comes up on tcp; the pool holds its base arena slab (the recv
 * buffers and a small allocation) and one oversize direct slab.  The direct
 * slab's registration is given a close that fails (its fid's ops replaced by
 * a copy whose close counts its calls and returns -FI_EBUSY), and the
 * transport is told its provider binds registrations to the endpoint, so
 * quiesce unregisters the pool before closing the endpoint.  Checks:
 *   1. quiesce returns false (inbound writes not shut out), having tried the
 *      failing close once; the direct slab keeps its handle and key, the base
 *      slab's registration closed (handle and key cleared); the endpoint, its
 *      CQ and its AV are kept, not closed and still named;
 *   2. reclaim keeps the recv buffers;
 *   3. a second unregister reports the kept registration again and does not
 *      retry its close;
 *   4. the pool cleanup does not retry the close and does not unmap the kept
 *      direct slab (written and read back after it).
 * The test then restores the real close and closes the registration, the
 * endpoint, the CQ and the AV itself, so the domain closes.
 *
 * Standalone: #includes net.c (its statics) with the shims net_rendezvous
 * uses, and links the real registered pool.  FI_PROVIDER=tcp is pinned by the
 * test's environment.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/shutdown.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

/* ---- runtime externs the module (and regpool) reference ------------------ */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1; /* self-only fabric */
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  exit(rc ? rc : 1);
}
void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t size) { return calloc(n, size); }
void *arts_calloc_aligned(size_t n, size_t size, size_t align) {
  void *p = NULL;
  if (posix_memalign(&p, align, n * size) != 0) {
    return NULL;
  }
  memset(p, 0, n * size);
  return p;
}
void arts_free(void *ptr) { free(ptr); }
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

#include "../../libs/src/core/transport/net.c"

#include "counter_stubs.h"
#include "net_runtime_stubs.h"

/* ------------------------------------------------------------------------- */

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define DIRECT_BYTES ((size_t)48 * 1024 * 1024) /* past the arena's ceiling */

static struct fi_ops g_failing_ops;
static struct fi_ops *g_real_ops;
static int g_close_calls;
static int g_failures;

#define FAIL(...)                                                              \
  do {                                                                         \
    printf("FAIL regpool_mr_close_kept: " __VA_ARGS__);                        \
    fflush(stdout);                                                            \
    g_failures++;                                                              \
  } while (0)

static int failing_close(struct fid *fid) {
  (void)fid;
  g_close_calls++;
  return -FI_EBUSY;
}

int main(void) {
  setenv("FI_PROVIDER", "tcp", 1);
  arts_net_init(NULL, NULL, NULL, false);
  if (!arts_regpool_init(arts_net_domain(), arts_net_mr_endpoint(), SLAB_BYTES,
                         0)) {
    printf("FAIL regpool_mr_close_kept: regpool init failed\n");
    return 1;
  }
  arts_net_rx_arm();

  unsigned char *small = (unsigned char *)arts_regpool_alloc_aligned(4096, 64);
  unsigned char *big =
      (unsigned char *)arts_regpool_alloc_aligned(DIRECT_BYTES, 64);
  if (small == NULL || big == NULL) {
    printf("FAIL regpool_mr_close_kept: regpool alloc failed\n");
    return 1;
  }
  const arts_regpool_mr_t *base_slab = arts_regpool_lookup(small);
  const arts_regpool_mr_t *direct_slab = arts_regpool_lookup(big);
  if (base_slab == NULL || direct_slab == NULL || base_slab == direct_slab ||
      base_slab->mr == NULL || direct_slab->mr == NULL) {
    printf("FAIL regpool_mr_close_kept: the two allocations do not lie in two "
           "registered slabs\n");
    return 1;
  }
  struct fid_mr *kept_mr = direct_slab->mr;
  uint64_t kept_key = direct_slab->rkey;
  g_real_ops = kept_mr->fid.ops;
  g_failing_ops = *g_real_ops;
  g_failing_ops.close = failing_close;
  kept_mr->fid.ops = &g_failing_ops;

  /* (1) quiesce, with the pool unregistered ahead of the endpoint close. */
  g_net.mr_mode |= FI_MR_ENDPOINT;
  if (arts_net_quiesce() || !g_net.writes_may_land) {
    FAIL("(1) quiesce reported inbound writes shut out after a registration "
         "did not close\n");
  }
  if (g_close_calls != 1) {
    FAIL("(1) the failing close ran %d time(s), want 1\n", g_close_calls);
  }
  if (direct_slab->mr != kept_mr || direct_slab->rkey != kept_key) {
    FAIL("(1) the registration that did not close lost its handle or key\n");
  }
  if (base_slab->mr != NULL || base_slab->rkey != 0) {
    FAIL("(1) the registration that closed kept its handle or key\n");
  }
  if (g_net.ep == NULL || g_net.cq == NULL || g_net.av == NULL) {
    FAIL("(1) quiesce closed or forgot the endpoint, its CQ or its AV\n");
  }

  /* (2) reclaim keeps what a write may still reach. */
  arts_net_reclaim();
  if (g_net.rxbuf[0] == NULL) {
    FAIL("(2) reclaim returned a recv buffer\n");
  }

  /* (3) a second unregister reports it again, without a second close. */
  if (arts_regpool_unregister()) {
    FAIL("(3) a second unregister reported every registration closed\n");
  }
  if (g_close_calls != 1) {
    FAIL("(3) the kept registration's close was retried\n");
  }

  /* (4) the cleanup neither retries the close nor unmaps the range. */
  arts_regpool_cleanup();
  if (g_close_calls != 1) {
    FAIL("(4) the cleanup retried the kept registration's close\n");
  }
  /* mincore fails with ENOMEM on a range that is not mapped. */
  unsigned char resident = 0;
  if (mincore(big, (size_t)sysconf(_SC_PAGESIZE), &resident) != 0) {
    FAIL("(4) the cleanup unmapped the kept direct slab\n");
  } else {
    memset(big, 0x5a, DIRECT_BYTES);
    if (big[0] != 0x5a || big[DIRECT_BYTES - 1] != 0x5a) {
      FAIL("(4) the kept direct slab does not hold what was written\n");
    }
  }

  /* The process exit would reclaim them; close them here so the domain
   * closes. */
  kept_mr->fid.ops = g_real_ops;
  int rc = fi_close(&kept_mr->fid);
  if (rc != 0) {
    FAIL("the real close of the kept registration failed: %s\n",
         fi_strerror(-rc));
  }
  /* The endpoint first: it holds the CQ and the AV. */
  struct fid *kept[] = {g_net.ep != NULL ? &g_net.ep->fid : NULL,
                        g_net.cq != NULL ? &g_net.cq->fid : NULL,
                        g_net.av != NULL ? &g_net.av->fid : NULL};
  for (unsigned i = 0; i < 3; i++) {
    rc = kept[i] != NULL ? fi_close(kept[i]) : 0;
    if (rc != 0) {
      FAIL("closing kept object %u failed: %s\n", i, fi_strerror(-rc));
    }
  }
  g_net.ep = NULL;
  g_net.cq = NULL;
  g_net.av = NULL;
  arts_net_teardown();

  if (g_failures != 0) {
    printf("FAIL regpool_mr_close_kept: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("PASS regpool_mr_close_kept\n");
  return 0;
}
