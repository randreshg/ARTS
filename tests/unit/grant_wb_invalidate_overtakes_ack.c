/* SPDX-License-Identifier: Apache-2.0
 *
 * Under write-back a standalone INVALIDATE can reach the new owner before its
 * CONFIRM_ACK: the home sends one as soon as a requester claims the baton
 * after the CONFIRM looked at the FIFO, and nothing orders it after the ack.
 * The new owner's word then still carries ARTS_GRANT_UNCONFIRMED, so the
 * INVALIDATE's withdrawal cannot reach zero, and the ack's guard removal (or
 * the last drained writer's release) owns the zero edge and ships to the
 * target the INVALIDATE published.
 *
 * This drives the owner side's handlers (coherence/grant_wb.c, with the RETAIN
 * count-dropping edges of coherence/grant_retain.c) on one thread with the
 * wire stubbed: the install's word (sentinel + drain guard + marker), then the
 * INVALIDATE, then a CONFIRM_ACK with no piggyback, with no drained writer and
 * with several, each released after the ack; and the control order, the ack
 * before the INVALIDATE.  Each must ship exactly once, to the INVALIDATE's
 * target, at the zero edge, and leave the word at zero.
 *
 * PURE test: no arts_rt, no ports, no threads.
 */
#include "coherence/grant_retain.c"
#include "coherence/grant_wb.c"
#include "coherence/grantreq_queue.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned int arts_global_rank_id = 1u;
unsigned int arts_global_rank_count = 3u;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { exit(code ? code : 1); }

#include "counter_stubs.h"

void *arts_malloc(size_t n) { return malloc(n); }
void arts_free(void *p) { free(p); }
unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return 0u;
}
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif
#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
#endif

/* The handlers of grant_wb.c and grant_retain.c this test does not drive.  */
#define UNREACHED(sig)                                                         \
  sig {                                                                        \
    printf("FAIL grant_wb_invalidate_overtakes_ack: %s reached\n", __func__);  \
    exit(1);                                                                   \
  }
UNREACHED(arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t g))
UNREACHED(arts_shared_ptr_t arts_db_cache_stub_install(arts_guid_t g,
                                                       uint64_t s))
UNREACHED(void arts_db_rdzv_discard_landing(uint64_t t, uint64_t s, uint64_t c))
UNREACHED(void arts_send_db_grant_confirm(unsigned int h, arts_guid_t g))
UNREACHED(void arts_send_db_grant_invalidate(
    unsigned int o, arts_guid_t g, unsigned int n,
    const struct arts_rdzv_landing_s *r, uint64_t h))
UNREACHED(void arts_db_grant_note_ex_holder(struct arts_db_s *db,
                                            unsigned int r))
UNREACHED(void arts_transport_send_async(int r, char *m, unsigned int n))
UNREACHED(void *arts_shared_get(arts_shared_ptr_t h))
UNREACHED(void arts_shared_release(arts_shared_ptr_t *p))
UNREACHED(bool arts_rank_u64_map_load(struct arts_rank_to_u64_map_s *m,
                                      const void *in, size_t size))
UNREACHED(void arts_net_rdzv_expect(uint64_t txid, uint64_t len,
                                    void (*on_data)(void *), void *arg))
UNREACHED(bool arts_db_buf_adopt_landing(struct arts_db_cache_s *c, uint64_t v,
                                         struct arts_db_buffer_s *l,
                                         uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install(
    struct arts_db_cache_s *c, uint64_t v, const void *d, uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install_landed(
    struct arts_db_cache_s *c, uint64_t v, struct arts_db_buffer_s *l,
    uint64_t s))

/* The drained writers: the drain gives each a hold, as the grant's drain
 * does for every woken waiter. */
static unsigned int g_drained;
void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *c) {
  atomic_fetch_add_explicit(&c->writer_count, g_drained, memory_order_acq_rel);
}

static int g_ships;
static unsigned int g_ship_to, g_ship_word;
void arts_db_grant_ship_pending(struct arts_db_cache_s *c) {
  g_ships++;
  g_ship_to = c->incoming_new_owner;
  g_ship_word = atomic_load_explicit(&c->writer_count, memory_order_acquire);
  c->incoming_new_owner = ARTS_NO_PENDING_OWNER;
}

#define TARGET 2u

static int scenario(int invalidate_first, unsigned int drained) {
  struct arts_db_s *db = calloc(1, sizeof(*db));
  struct arts_db_cache_s *c = &db->cache;
  c->incoming_new_owner = ARTS_NO_PENDING_OWNER;
  /* The install: sentinel + drain guard, unconfirmed. */
  atomic_init(&c->writer_count, 2u + ARTS_GRANT_UNCONFIRMED);
  g_drained = drained;
  g_ships = 0;
  g_ship_to = g_ship_word = ~0u;

  struct arts_ooo_args_db_grant_invalidate_s inv;
  memset(&inv, 0, sizeof(inv));
  inv.new_owner_rank = TARGET;
  inv.new_owner_have = ARTS_GRANT_VERSION_NONE;
  struct arts_msg_grant_confirm_ack_packet_s ack;
  memset(&ack, 0, sizeof(ack));
  ack.new_owner_rank = ARTS_NO_PENDING_OWNER;
  ack.new_owner_have = ARTS_GRANT_VERSION_NONE;

  int early = 0;
  if (invalidate_first) {
    arts_handler_db_grant_invalidate(db, &inv);
    early |= g_ships;
    arts_handler_db_grant_confirm_ack(db, &ack);
  } else {
    arts_handler_db_grant_confirm_ack(db, &ack);
    early |= g_ships && drained > 0u;
    arts_handler_db_grant_invalidate(db, &inv);
  }
  for (unsigned int i = 0; i < drained; i++) {
    early |= g_ships;
    arts_db_grant_release_commit(c);
  }
  unsigned int w = atomic_load_explicit(&c->writer_count, memory_order_acquire);
  int ok = !early && g_ships == 1 && g_ship_to == TARGET && g_ship_word == 0u &&
           w == 0u;
  if (!ok) {
    printf("FAIL grant_wb_invalidate_overtakes_ack: %s, %u drained writers: "
           "ships=%d to=%u at word=0x%x, word=0x%x, early=%d\n",
           invalidate_first ? "INVALIDATE before the ack"
                            : "ack before the INVALIDATE",
           drained, g_ships, g_ship_to, g_ship_word, w, early);
  }
  free(db);
  return ok ? 0 : 1;
}

int main(void) {
  int rc = 0;
  for (unsigned int k = 0; k <= 3u; k++) {
    rc |= scenario(1, k);
    rc |= scenario(0, k);
  }
  if (rc == 0) {
    printf("PASS grant_wb_invalidate_overtakes_ack: an INVALIDATE before or "
           "after the CONFIRM_ACK ships once, at the zero edge\n");
  }
  return rc;
}
