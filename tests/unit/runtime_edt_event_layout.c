/* SPDX-License-Identifier: Apache-2.0
 *
 * C-vs-C++ layout parity of the protocol-independent runtime structs a C++
 * translation unit (the CUDA sources, and the C sources a GPU build compiles
 * as CUDA) shares with the C runtime, + arts_edt_total_size arithmetic.
 *
 * Property under test
 * -------------------
 * Each struct has ONE definition.  Its atomic members are spelled
 * `_Atomic(T)`, which the C++ side of arts/utils/stdatomic_compat.h maps to
 * `std::atomic<T>`, and a C++ translation unit exchanges these structs with
 * the C runtime.  The two compilations must therefore lay every struct out
 * byte for byte alike: a member whose `std::atomic<T>` differs from
 * `_Atomic(T)` in size or alignment silently corrupts whatever the two sides
 * share.  Covered: the lock-free primitives (link, stack, MPSC queue, tagged
 * pool, shared-pointer slot, rank bit-set), the EDT and event descriptors,
 * the dependence slot and its acquire record, the event dependence node, the
 * route table and its items, the runtime's shared, per-thread and shutdown
 * state, and the counter cell.  The DB descriptors depend on the coherence
 * cell and have their own probe (db_cache_layout).  `arts_edt_total_size`'s
 * trailing `[paramv | depv]` arithmetic is load-bearing for every EDT
 * allocation/copy.
 *
 * How parity is checked
 * -------------------
 * This source file is compiled BOTH as C and as C++; each prints one line of
 * sizes, alignments and member offsets.  ctest registers two executables
 * from this one file and a third "compare" step compares their printed
 * lines, so a drift between the two compilations is caught.  Independently,
 * each embeds compile-time asserts and runtime checks on the invariants that
 * MUST hold regardless of host:
 *   - sizeof(arts_event_s) is a multiple of its alignment and >= a guid;
 *   - arts_edt_total_size(edt) == sizeof(struct) + paramc*8 + depc*sizeof(dep)
 *     for a matrix of (paramc, depc) including 0/0 and large values;
 *   - both structs honor ARTS_ALIGNED_MAX (cache-line alignment).
 */

#include "arts/db.h"
#include "arts/event.h"
#include "arts/gas/route_table.h"
#include "arts/rank_bitset.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/utils/lockfree_pool.h"
#include "arts/utils/mpsc.h"
#include "arts/utils/shared.h"

#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#define LAYOUT_LANG "cpp"
#else
#define LAYOUT_LANG "c"
#endif

/* One struct's size and alignment, then one member's offset and size. */
#define TYPE(t) printf(" " #t "=%zu/%zu", sizeof(t), (size_t)alignof(t))
#define MEMBER(t, m)                                                           \
  printf(" " #t "." #m "=%zu/%zu", offsetof(t, m), sizeof(((t *)0)->m))

/* ---- compile-time invariants (per compilation; drift fires here) --------- */

/* Discriminator is the first byte. */
static_assert(offsetof(struct arts_event_s, is_channel) == 0,
              "is_channel must be first");

/* EDT layout is fixed-prefix; the trailing arrays are computed, never declared,
 * so sizeof(struct arts_edt_s) must be exactly the prefix size. */

static void check_event_layout(void) {
  /* Structural facts that pin the layout regardless of host: the whole
   * struct is a multiple of its alignment, cache-line aligned, and at least
   * large enough for a guid. */
  size_t sz = sizeof(struct arts_event_s);
  size_t al = alignof(struct arts_event_s);
  if (al == 0 || (sz % al) != 0) {
    fprintf(stderr,
            "FAIL runtime_edt_event_layout[%s]: sizeof(arts_event_s)=%zu not "
            "multiple of align=%zu\n",
            LAYOUT_LANG, sz, al);
    exit(1);
  }
  /* ARTS_ALIGNED_MAX pins shared objects to a full cache line
   * (ARTS_CACHE_LINE_SIZE) for false-sharing isolation. */
  if (al != ARTS_CACHE_LINE_SIZE) {
    fprintf(stderr,
            "FAIL runtime_edt_event_layout[%s]: arts_event_s align=%zu != "
            "ARTS_CACHE_LINE_SIZE %d\n",
            LAYOUT_LANG, al, (int)ARTS_CACHE_LINE_SIZE);
    exit(1);
  }
  if (sz < sizeof(arts_guid_t)) {
    fprintf(stderr,
            "FAIL runtime_edt_event_layout[%s]: arts_event_s smaller than a "
            "guid (%zu)\n",
            LAYOUT_LANG, sz);
    exit(1);
  }
}

static void check_edt_total_size(void) {
  /* arts_edt_total_size = sizeof(struct) + paramc*8 + depc*sizeof(dep). */
  const uint64_t base = (uint64_t)sizeof(struct arts_edt_s);
  const uint64_t depsz = (uint64_t)sizeof(arts_edt_dep_t);

  /* sizeof(struct arts_edt_s) must honor the cache-line alignment too. */
  if (alignof(struct arts_edt_s) != ARTS_CACHE_LINE_SIZE) {
    fprintf(stderr,
            "FAIL runtime_edt_event_layout[%s]: arts_edt_s align=%zu != "
            "ARTS_CACHE_LINE_SIZE %d\n",
            LAYOUT_LANG, (size_t)alignof(struct arts_edt_s),
            (int)ARTS_CACHE_LINE_SIZE);
    exit(1);
  }

  const uint32_t pcs[] = {0, 1, 2, 7, 16, 1024, 65535};
  const uint32_t dcs[] = {0, 1, 3, 8, 32, 4096, 65535};
  for (size_t i = 0; i < sizeof(pcs) / sizeof(pcs[0]); i++) {
    for (size_t j = 0; j < sizeof(dcs) / sizeof(dcs[0]); j++) {
      struct arts_edt_s edt;
      memset((void *)&edt, 0, sizeof(edt));
      edt.paramc = pcs[i];
      edt.depc = dcs[j];
      uint64_t got = arts_edt_total_size(&edt);
      uint64_t want = base + (uint64_t)pcs[i] * 8u + (uint64_t)dcs[j] * depsz;
      if (got != want) {
        fprintf(stderr,
                "FAIL runtime_edt_event_layout[%s]: "
                "total_size(paramc=%u,depc=%u)=%llu want %llu "
                "(base=%llu depsz=%llu)\n",
                LAYOUT_LANG, pcs[i], dcs[j], (unsigned long long)got,
                (unsigned long long)want, (unsigned long long)base,
                (unsigned long long)depsz);
        exit(1);
      }
    }
  }

  /* paramv slot is exactly 8 bytes per the helper's *8 — assert that promise.
   */
  struct arts_edt_s a, b;
  memset((void *)&a, 0, sizeof(a));
  memset((void *)&b, 0, sizeof(b));
  a.paramc = 0;
  a.depc = 0;
  b.paramc = 1;
  b.depc = 0;
  if (arts_edt_total_size(&b) - arts_edt_total_size(&a) != 8u) {
    fprintf(stderr,
            "FAIL runtime_edt_event_layout[%s]: paramv slot not 8 bytes\n",
            LAYOUT_LANG);
    exit(1);
  }
  b.paramc = 0;
  b.depc = 1;
  if (arts_edt_total_size(&b) - arts_edt_total_size(&a) != depsz) {
    fprintf(stderr,
            "FAIL runtime_edt_event_layout[%s]: depv slot != "
            "sizeof(arts_edt_dep_t)\n",
            LAYOUT_LANG);
    exit(1);
  }
}

typedef struct arts_edt_s edt_t;
typedef struct arts_dep_acq_s dep_acq_t;
typedef struct arts_event_s event_t;
typedef struct arts_event_dep_s event_dep_t;
typedef struct arts_rank_bitset_s rank_bitset_t;
typedef struct arts_route_table_s route_table_t;
typedef struct arts_runtime_shared_s runtime_shared_t;
typedef struct arts_runtime_private_s runtime_private_t;
typedef struct arts_shutdown_abandon_s shutdown_abandon_t;

static void print_fingerprint(void) {
  /* Every load-bearing offset, so the sibling compilation can be compared
   * byte for byte.  Size/alignment alone cannot detect two members moving
   * within unchanged padding. */
  printf("EDT_EVENT_LAYOUT %s", LAYOUT_LANG);

  TYPE(arts_lf_link_t);
  MEMBER(arts_lf_link_t, next);
  TYPE(arts_lf_stack_t);
  MEMBER(arts_lf_stack_t, head);
  TYPE(arts_mpsc_t);
  MEMBER(arts_mpsc_t, head);
  MEMBER(arts_mpsc_t, tail);
  MEMBER(arts_mpsc_t, stub);
  TYPE(arts_lf_pool_head_t);
  TYPE(arts_lockfree_pool_t);
  MEMBER(arts_lockfree_pool_t, head);
  TYPE(arts_shared_slot_t);
  TYPE(_Atomic(arts_shared_slot_t));
  TYPE(rank_bitset_t);
  MEMBER(rank_bitset_t, words);
  MEMBER(rank_bitset_t, nranks);
  MEMBER(rank_bitset_t, nwords);

  TYPE(edt_t);
  TYPE(arts_edt_dep_t);
  MEMBER(arts_edt_dep_t, guid);
  MEMBER(arts_edt_dep_t, ptr);
  MEMBER(arts_edt_dep_t, mode);
  TYPE(dep_acq_t);
  MEMBER(dep_acq_t, ptr);
  MEMBER(dep_acq_t, pin);
  MEMBER(dep_acq_t, subtype);
  MEMBER(dep_acq_t, alias);
  MEMBER(dep_acq_t, resolved);
  MEMBER(edt_t, func_ptr);
  MEMBER(edt_t, paramc);
  MEMBER(edt_t, depc);
  MEMBER(edt_t, guid);
  MEMBER(edt_t, finish_event);
  MEMBER(edt_t, output_event);
  MEMBER(edt_t, output_data);
  MEMBER(edt_t, edt_type);
  MEMBER(edt_t, depc_needed);
  MEMBER(edt_t, rw_cursor);
  MEMBER(edt_t, rw_sorted);
  MEMBER(edt_t, dep_acq);
  MEMBER(edt_t, acquire_remaining);
  MEMBER(edt_t, invalidate_count);
  MEMBER(edt_t, self_cb);

  TYPE(event_t);
  MEMBER(event_t, is_channel);
  MEMBER(event_t, simple);
  MEMBER(event_t, simple.state);
  MEMBER(event_t, simple.counted);
  MEMBER(event_t, simple.auto_destroy);
  MEMBER(event_t, simple.discard_data);
  MEMBER(event_t, simple.data);
  MEMBER(event_t, simple.deps_stack);
  MEMBER(event_t, channel);
  MEMBER(event_t, channel.state);
  MEMBER(event_t, channel.data_queue);
  MEMBER(event_t, channel.dep_queue);
  TYPE(event_dep_t);
  MEMBER(event_dep_t, link);
  MEMBER(event_dep_t, target);
  MEMBER(event_dep_t, slot);

  TYPE(arts_route_item_t);
  MEMBER(arts_route_item_t, key);
  MEMBER(arts_route_item_t, value);
  MEMBER(arts_route_item_t, ooo_list);
  MEMBER(arts_route_item_t, claim_key);
  MEMBER(arts_route_item_t, claim_ver);
  TYPE(route_table_t);
  MEMBER(route_table_t, data);
  MEMBER(route_table_t, size);
  MEMBER(route_table_t, next);
  MEMBER(route_table_t, ops);

  TYPE(runtime_shared_t);
  MEMBER(runtime_shared_t, scheduler);
  MEMBER(runtime_shared_t, route_table);
  MEMBER(runtime_shared_t, local_spin);
  MEMBER(runtime_shared_t, ready_to_push);
  MEMBER(runtime_shared_t, ready_to_parallel_start);
  MEMBER(runtime_shared_t, ready_to_inspect);
  MEMBER(runtime_shared_t, ready_to_clean);
  MEMBER(runtime_shared_t, shutdown_state);
  MEMBER(runtime_shared_t, shutdown_entry_done);
  MEMBER(runtime_shared_t, e2e_start_stamp);
  MEMBER(runtime_shared_t, e2e_marker_enabled);
  MEMBER(runtime_shared_t, db_rr_route);
  MEMBER(runtime_shared_t, edt_rr_route);
  MEMBER(runtime_shared_t, keys);
  MEMBER(runtime_shared_t, counter_capture_interval);
  TYPE(runtime_private_t);
  MEMBER(runtime_private_t, my_deque);
  MEMBER(runtime_private_t, alive);
  MEMBER(runtime_private_t, rng_key);
  TYPE(shutdown_abandon_t);
  MEMBER(shutdown_abandon_t, queued_edts);
  MEMBER(shutdown_abandon_t, waits);
  MEMBER(shutdown_abandon_t, unjoined_threads);
  MEMBER(shutdown_abandon_t, cancelled_threads);
  TYPE(arts_counter_t);
  MEMBER(arts_counter_t, count);
  MEMBER(arts_counter_t, start);

  printf("\n");
}

int main(void) {
  check_event_layout();
  check_edt_total_size();
  print_fingerprint();
  printf("PASS runtime_edt_event_layout [%s]: runtime layouts + total_size "
         "arithmetic verified\n",
         LAYOUT_LANG);
  return 0;
}
