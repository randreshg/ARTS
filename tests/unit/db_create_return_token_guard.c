/* SPDX-License-Identifier: Apache-2.0
 *
 * A create's credit is applied only to the descriptor whose create it answers.
 *
 * The home answers a remote create with the creator's first release credit,
 * echoing the name the creator gave its descriptor.  By the time the answer
 * arrives the GUID may name a later descriptor (the block was destroyed and
 * created again), and the credit then names storage of the earlier block, so
 * the receiver must apply it only where the name still matches.  A zero txid
 * is "no credit" and applies nothing whatever the name.
 *
 * Whitebox, one rank: the answer is a hand-built packet fed to the dispatcher
 * body against a descriptor this rank created; the descriptor's name is set by
 * hand (a name is minted only where the creator is not the home), and every
 * field the test touches is restored before the block is destroyed.
 */
#include <stdint.h>
#include <stdio.h>

#include "arts.h"
#include "../test_failure_status.h"

#if !((!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WT)) ||      \
      defined(ARTS_PROTOCOL_FLUSH))
int main(void) {
  printf("SKIP db_create_return_token_guard: no create credit on this arm\n");
  return 0;
}
#else

#include "arts/coherence/types.h"
#include "arts/gas/route_table.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"
#include "arts/utils/shared.h"

#define TOKEN UINT64_C(0x0003000000000011)
#define OTHER UINT64_C(0x0003000000000012)
#define ADDR UINT64_C(0x00007f00deadbe00)
#define RKEY UINT64_C(0x0000000000abcd01)
#define TXID UINT64_C(0x00000000000077a5)

struct credit {
  uint64_t addr, rkey, txid;
};

static struct credit read_credit(struct arts_db_cache_s *c) {
#if defined(ARTS_PROTOCOL_FLUSH)
  return (struct credit){
      __atomic_load_n(&c->home_line_addr, __ATOMIC_RELAXED),
      __atomic_load_n(&c->home_line_rkey, __ATOMIC_RELAXED),
      __atomic_load_n(&c->flush_txid, __ATOMIC_ACQUIRE)};
#else
  return (struct credit){
      __atomic_load_n(&c->home_pub_addr, __ATOMIC_RELAXED),
      __atomic_load_n(&c->home_pub_rkey, __ATOMIC_RELAXED),
      __atomic_load_n(&c->home_pub_txid, __ATOMIC_ACQUIRE)};
#endif
}

static void write_credit(struct arts_db_cache_s *c, struct credit v) {
#if defined(ARTS_PROTOCOL_FLUSH)
  __atomic_store_n(&c->home_line_addr, v.addr, __ATOMIC_RELAXED);
  __atomic_store_n(&c->home_line_rkey, v.rkey, __ATOMIC_RELAXED);
  __atomic_store_n(&c->flush_txid, v.txid, __ATOMIC_RELEASE);
#else
  __atomic_store_n(&c->home_pub_addr, v.addr, __ATOMIC_RELAXED);
  __atomic_store_n(&c->home_pub_rkey, v.rkey, __ATOMIC_RELAXED);
  __atomic_store_n(&c->home_pub_txid, v.txid, __ATOMIC_RELEASE);
#endif
}

static void deliver(arts_guid_t db, uint64_t token, uint64_t txid) {
  struct arts_msg_db_create_return_packet_s p;
  arts_fill_packet_header(&p.header, (uint64_t)sizeof(p),
                          (unsigned int)MSG_DB_CREATE_RETURN);
  p.db_guid = db;
  p.create_token = token;
  p.credit_addr = ADDR;
  p.credit_rkey = RKEY;
  p.credit_txid = txid;
  arts_transport_dispatch_body(&p.header);
}

static int same(struct credit a, struct credit b) {
  return a.addr == b.addr && a.rkey == b.rkey && a.txid == b.txid;
}

static void check(int ok, const char *what, struct credit got) {
  if (ok) {
    arts_printf("ok: %s\n", what);
    return;
  }
  arts_test_fail();
  arts_printf("FAIL: db_create_return_token_guard %s (addr=0x%llx rkey=0x%llx "
              "txid=0x%llx)\n",
              what, (unsigned long long)got.addr,
              (unsigned long long)got.rkey, (unsigned long long)got.txid);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;

  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NO_ACQUIRE, NULL);
  arts_shared_ptr_t h = arts_route_table_lookup_db(db);
  struct arts_db_s *d = (struct arts_db_s *)arts_shared_get(h);
  if (d == NULL) {
    arts_test_fail();
    arts_printf("FAIL: db_create_return_token_guard found no descriptor\n");
    arts_shared_release(&h);
    arts_shutdown();
    return;
  }
  struct arts_db_cache_s *c = &d->cache;
  uint64_t token0 = __atomic_load_n(&c->create_token, __ATOMIC_RELAXED);
  struct credit orig = read_credit(c);
  struct credit zero = {0, 0, 0};
  struct credit sent = {ADDR, RKEY, TXID};

  __atomic_store_n(&c->create_token, TOKEN, __ATOMIC_RELEASE);
  write_credit(c, zero);

  deliver(db, OTHER, TXID);
  struct credit got = read_credit(c);
  check(same(got, zero), "a foreign name leaves the credit untouched", got);

  deliver(db, TOKEN, 0);
  got = read_credit(c);
  check(same(got, zero), "a zero txid applies nothing under a matching name",
        got);

  deliver(db, OTHER, 0);
  got = read_credit(c);
  check(same(got, zero), "a zero txid applies nothing under a foreign name",
        got);

  deliver(db, TOKEN, TXID);
  got = read_credit(c);
  check(same(got, sent), "a matching name applies the credit as sent", got);

  write_credit(c, orig);
  __atomic_store_n(&c->create_token, token0, __ATOMIC_RELEASE);
  arts_shared_release(&h);

  if (arts_test_status() == 0) {
    arts_printf("PASS: db_create_return_token_guard\n");
  }
  arts_db_destroy(db);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
#endif
