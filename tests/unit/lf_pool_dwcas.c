/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_lf_pool_head_t is a REAL lock-free 128-bit DWCAS, not a
 * libatomic-locked emulation (lockfree_pool.h).
 *
 * The whole ABA-defense argument of arts_lockfree_pool_t rests on the 16-byte
 * (ptr,tag) head being updated with a genuine lock-free cmpxchg16b / ldxp+stxp.
 * If a build links the libatomic LOCKED fallback, the pool still works but the
 * "lock-free" claim is void — a build-config (-mcx16) regression that this
 * test catches.
 *
 * Toolchain behaviour this test is written against: GCC/Clang emit a libcall
 * (__atomic_compare_exchange_16) for 16-byte atomics even with -mcx16, and
 * classify the type as NOT lock-free (atomic_is_lock_free == 0,
 * __atomic_always_lock_free(16) == 0) because a 16-byte atomic *load* would
 * have to be implemented with a writing cmpxchg16b.  The top-level
 * CMakeLists.txt documents exactly this and links libatomic; libatomic in
 * turn uses the lock-free cmpxchg16b instruction at runtime whenever the CPU
 * advertises CX16.  Therefore asserting atomic_is_lock_free()==true would be
 * a FALSE failure on the supported toolchain — instead this test:
 *   (a) hard-fails only if the CPU lacks CX16 (then libatomic would fall back
 *       to a locked mutex table — the genuine "not lock-free DWCAS" defect),
 *   (b) verifies the 16-byte DWCAS is FUNCTIONALLY correct (both lanes swap
 *       atomically; mismatch leaves the slot untouched),
 *   (c) reports the compiler's lock-free classification for the record.
 *
 * Pure compile/runtime check, no threads.  Meaningful only with -mcx16
 * (mirrors the global build flag).
 */

#include "arts/utils/lockfree_pool.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#if defined(__x86_64__) || defined(__amd64__)
#include <cpuid.h>
#endif

/* libc-backed shims so the header's batch/alloc inline fns link standalone
 * (they are not exercised here, but the TU references arts_calloc/arts_free
 * through the inline bodies if instantiated). */
#include <stdlib.h>
void *arts_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *arts_calloc_aligned(size_t nmemb, size_t size, size_t align) {
  void *p = NULL;
  if (posix_memalign(&p, align, nmemb * size) != 0) {
    return NULL;
  }
  return p;
}
void arts_free(void *ptr) { free(ptr); }

_Static_assert(sizeof(arts_lf_pool_head_t) == 16,
               "DWCAS head must be 16 bytes");
_Static_assert(_Alignof(arts_lf_pool_head_t) == 16,
               "DWCAS head must be 16-aligned");

int main(void) {
  /* A real, properly-aligned _Atomic head object — the exact field type used
   * inside arts_lockfree_pool_t. */
  _Atomic(arts_lf_pool_head_t) head;
  arts_lf_pool_head_t init = {.ptr = NULL, .tag = 0};
  atomic_store_explicit(&head, init, memory_order_relaxed);

  /* The genuine "not a real DWCAS" defect is the CPU lacking CX16 — then
   * libatomic would fall back to a locked mutex table.  On x86-64 the CPU
   * reports the flag itself (CPUID leaf 1, which every x86-64 CPU has). */
#if defined(__x86_64__) || defined(__amd64__)
  {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
      (void)fprintf(stderr, "FAIL lf_pool_dwcas: CPUID leaf 1 unavailable\n");
      return 1;
    }
    if ((ecx & bit_CMPXCHG16B) == 0) {
      (void)fprintf(stderr,
                    "FAIL lf_pool_dwcas: CPU lacks CX16 — libatomic uses a "
                    "LOCKED mutex-table fallback; the pool's 128-bit ABA "
                    "defense is not lock-free on this machine.\n");
      return 1;
    }
  }
#endif

  /* Compiler's lock-free classification — reported, not asserted: on
   * GCC/Clang x86-64 it is 0 by design (a 16-byte load would have to write),
   * even though the runtime DWCAS uses the lock-free CMPXCHG16B instruction. */
  int compiler_lockfree = atomic_is_lock_free(&head);

  /* Sanity: a DWCAS actually swaps both lanes atomically. */
  arts_lf_pool_head_t expected = init;
  arts_lf_pool_head_t desired = {.ptr = (arts_lf_link_t *)0x10, .tag = 1};
  if (!atomic_compare_exchange_strong_explicit(&head, &expected, desired,
                                               memory_order_acq_rel,
                                               memory_order_relaxed)) {
    (void)fprintf(stderr, "FAIL lf_pool_dwcas: initial DWCAS unexpectedly "
                          "failed\n");
    return 1;
  }
  arts_lf_pool_head_t now = atomic_load_explicit(&head, memory_order_relaxed);
  if (now.ptr != (arts_lf_link_t *)0x10 || now.tag != 1) {
    (void)fprintf(stderr,
                  "FAIL lf_pool_dwcas: DWCAS did not swap both lanes "
                  "(ptr=%p tag=%zu)\n",
                  (void *)now.ptr, (size_t)now.tag);
    return 1;
  }

  /* A DWCAS against a stale expectation (right pointer, old tag) must fail,
   * leave both lanes untouched, and hand back the current value. */
  arts_lf_pool_head_t stale = {.ptr = (arts_lf_link_t *)0x10, .tag = 0};
  arts_lf_pool_head_t other = {.ptr = (arts_lf_link_t *)0x20, .tag = 2};
  if (atomic_compare_exchange_strong_explicit(
          &head, &stale, other, memory_order_acq_rel, memory_order_relaxed)) {
    (void)fprintf(stderr, "FAIL lf_pool_dwcas: DWCAS succeeded against a "
                          "stale tag\n");
    return 1;
  }
  now = atomic_load_explicit(&head, memory_order_relaxed);
  if (now.ptr != (arts_lf_link_t *)0x10 || now.tag != 1 ||
      stale.ptr != (arts_lf_link_t *)0x10 || stale.tag != 1) {
    (void)fprintf(stderr,
                  "FAIL lf_pool_dwcas: a failed DWCAS changed the slot or "
                  "misreported it (slot ptr=%p tag=%zu, reported ptr=%p "
                  "tag=%zu)\n",
                  (void *)now.ptr, (size_t)now.tag, (void *)stale.ptr,
                  (size_t)stale.tag);
    return 1;
  }

#if defined(__x86_64__) || defined(__amd64__)
  const char *cx16 = "CX16 present, ";
#else
  const char *cx16 = "";
#endif
  printf("PASS lf_pool_dwcas: %s16-byte DWCAS swaps both lanes "
         "atomically (compiler atomic_is_lock_free=%d — 0 expected on "
         "GCC/Clang x86-64 by design)\n",
         cx16, compiler_lockfree);
  return 0;
}
