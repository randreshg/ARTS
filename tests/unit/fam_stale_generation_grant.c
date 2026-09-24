/// @file fam_stale_generation_grant.c
/// @brief A grant naming a store other than the one this rank's cache names is
/// refused, loudly; the same store named again is not.
///
/// One store per generation, minted by that generation's creator, so a cache
/// can meet a second address only when a later generation's grant reaches a
/// cache of an earlier one.  That cache's store has been freed by the earlier
/// generation's teardown and may belong to another block by then, so reading
/// or writing through it is never an option and dropping the grant would
/// strand the home's count: the runtime stops.
///
/// Whitebox, because reaching that shape through the public API needs a
/// message order no program can steer.  The recorder's three answers are
/// asserted directly on a live block's cache.  The refusal is asserted by
/// applying a grant naming a foreign store through the grant handler in a
/// forked child and reading back its exit status and its diagnostic.

#include "arts.h"

#include "arts/coherence/coherence.h"
#include "arts/coherence/excl/types.h"
#include "arts/coherence/handlers.h"
#include "arts/gas/route_table.h"
#include "arts/transport/protocol.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BYTES 256u
#define REFUSAL "a later generation's grant"

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL fam_stale_generation_grant: %s\n", what);
  arts_test_fail();
}

/* Runs the grant handler in a child whose output is captured in buf; returns
 * the child's wait status. */
static int grant_in_child(arts_guid_t g, uint64_t addr, char *buf,
                          size_t cap) {
  int fds[2];
  if (pipe(fds) != 0) {
    return -1;
  }
  (void)fflush(stdout);
  (void)fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    (void)close(fds[0]);
    (void)dup2(fds[1], STDOUT_FILENO);
    (void)dup2(fds[1], STDERR_FILENO);
    (void)alarm(10); /* a handler that went on instead of refusing */
    struct arts_msg_excl_grant_packet_s p;
    memset(&p, 0, sizeof(p));
    p.db_guid = g;
    p.mode = (uint32_t)DB_MODE_RW;
    p.data_size = BYTES;
    p.pub.addr = addr;
    arts_handler_db_excl_grant(&p, sizeof(p));
    _exit(0);
  }
  (void)close(fds[1]);
  size_t n = 0;
  ssize_t r;
  while (n + 1 < cap && (r = read(fds[0], buf + n, cap - 1 - n)) > 0) {
    n += (size_t)r;
  }
  buf[n] = '\0';
  (void)close(fds[0]);
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid) {
    return -1;
  }
  return status;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, arts_get_current_rank());
  (void)arts_db_create_with_guid(label, BYTES, ARTS_DB,
                                 ARTS_DB_PROP_NO_ACQUIRE, NULL);

  arts_shared_ptr_t h = arts_route_table_lookup_db(label);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db == NULL) {
    fail("a created block is not installed at its home");
    arts_shared_release(&h);
    arts_shutdown();
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  uint64_t mine = arts_db_fam_slot_addr(cache);
  if (mine == 0) {
    fail("a sized block has no store");
  }
  /* Any other address will do: the refusal must fire before anything reads
   * through it. */
  uint64_t other = mine + 64u;

  if (arts_db_fam_slot_record(cache, mine) != ARTS_FAM_SLOT_KNOWN) {
    fail("the store a cache already names was not reported as known");
  }
  if (arts_db_fam_slot_record(cache, 0) != ARTS_FAM_SLOT_KNOWN) {
    fail("no address at all was not reported as known");
  }
  if (arts_db_fam_slot_record(cache, other) != ARTS_FAM_SLOT_CONFLICT) {
    fail("a second store for one cache was not reported as a conflict");
  }
  if (arts_db_fam_slot_addr(cache) != mine) {
    fail("a conflicting address replaced the store a cache names");
  }
  struct arts_db_cache_s fresh;
  memset(&fresh, 0, sizeof(fresh));
  if (arts_db_fam_slot_record(&fresh, mine) != ARTS_FAM_SLOT_RECORDED ||
      arts_db_fam_slot_addr(&fresh) != mine) {
    fail("a cache with no store did not record the one it was given");
  }
  arts_shared_release(&h);

  char out[4096];
  int status = grant_in_child(label, other, out, sizeof(out));
  if (status == -1) {
    fail("the child could not be run");
  } else if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) {
    fail("a grant naming a foreign store was applied");
  } else if (strstr(out, REFUSAL) == NULL) {
    fail("the refusal does not name its cause");
  }

  if (arts_test_status() == 0) {
    printf("PASS fam_stale_generation_grant\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
