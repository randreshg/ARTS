/* Hermetic tests for the registered pool's per-node availability estimate
 * (arts_regpool_parse_node_avail): the parser is pure over a stdio stream,
 * so every policy claim is checked here against synthetic meminfo text.  The
 * claim is that the estimate is the node's FREE pages and nothing else — a
 * mapping placed on a node by preference takes its free pages and spills
 * past them rather than reclaiming that node's file cache, so cache is not
 * room the mapping can take locally — plus the contract that an unreadable
 * MemFree must not veto placement. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arts/memory/regpool.h"

static size_t parse(const char *text) {
  FILE *f = fmemopen((void *)text, strlen(text), "r");
  assert(f != NULL);
  size_t r = arts_regpool_parse_node_avail(f);
  fclose(f);
  return r;
}

int main(void) {
  /* Cache-heavy node: tens of GiB on the file LRUs must NOT be counted —
   * they are not room a preferred mapping takes without leaving the node. */
  {
    const char *t = "Node 6 MemTotal:       130829040 kB\n"
                    "Node 6 MemFree:        150000 kB\n"
                    "Node 6 MemUsed:        130679040 kB\n"
                    "Node 6 Active(anon):   1000000 kB\n"
                    "Node 6 Inactive(anon): 2000000 kB\n"
                    "Node 6 Active(file):   24661016 kB\n"
                    "Node 6 Inactive(file): 47900732 kB\n"
                    "Node 6 Unevictable:    32 kB\n"
                    "Node 6 Dirty:          84 kB\n"
                    "Node 6 Writeback:      0 kB\n"
                    "Node 6 NFS_Unstable:   0 kB\n"
                    "Node 6 WritebackTmp:   0 kB\n"
                    "Node 6 HugePages_Total: 0\n";
    assert(parse(t) == (size_t)150000 * 1024);
  }

  /* Anon-full node: same answer, reached the other way. */
  {
    const char *t = "Node 2 MemFree:        150000 kB\n"
                    "Node 2 Active(file):   0 kB\n"
                    "Node 2 Inactive(file): 0 kB\n"
                    "Node 2 Dirty:          0 kB\n";
    assert(parse(t) == (size_t)150000 * 1024);
  }

  /* Older field sets carrying no file LRUs at all: unchanged answer. */
  {
    const char *t = "Node 0 MemTotal:       1000000 kB\n"
                    "Node 0 MemFree:        123456 kB\n"
                    "Node 0 MemUsed:        876544 kB\n";
    assert(parse(t) == (size_t)123456 * 1024);
  }

  /* No MemFree at all / empty stream: unknown must not veto placement. */
  assert(parse("Node 0 MemTotal: 1 kB\n") == SIZE_MAX);
  assert(parse("") == SIZE_MAX);

  /* Field-name discrimination: no other field may be mistaken for MemFree,
   * and suffix-less lines must not derail the scan. */
  {
    const char *t = "Node 1 Active:         777777 kB\n"
                    "Node 1 Active(anon):   888888 kB\n"
                    "Node 1 HugePages_Total: 0\n"
                    "Node 1 MemFree:        1000 kB\n"
                    "Node 1 Active(file):   2000 kB\n"
                    "Node 1 Inactive(file): 2000 kB\n";
    assert(parse(t) == (size_t)1000 * 1024);
  }

  /* A complete real per-node meminfo (captured verbatim): the scan must hold
   * against the full production field set — MemTotal, MemUsed, FilePages,
   * SReclaimable, Shmem, plain Active/Inactive and the (anon)/(file)
   * variants must all be ignored. */
  {
    const char *t =
        "Node 0 MemTotal:       131792596 kB\n"
        "Node 0 MemFree:        108191140 kB\n"
        "Node 0 MemUsed:        23601456 kB\n"
        "Node 0 SwapCached:            0 kB\n"
        "Node 0 Active:          1510648 kB\n"
        "Node 0 Inactive:       18994656 kB\n"
        "Node 0 Active(anon):      63292 kB\n"
        "Node 0 Inactive(anon):  4427004 kB\n"
        "Node 0 Active(file):    1447356 kB\n"
        "Node 0 Inactive(file): 14567652 kB\n"
        "Node 0 Unevictable:        3072 kB\n"
        "Node 0 Mlocked:               0 kB\n"
        "Node 0 Dirty:                 0 kB\n"
        "Node 0 Writeback:             0 kB\n"
        "Node 0 FilePages:      16317620 kB\n"
        "Node 0 Mapped:           190428 kB\n"
        "Node 0 AnonPages:       4188748 kB\n"
        "Node 0 Shmem:            302652 kB\n"
        "Node 0 KernelStack:       22612 kB\n"
        "Node 0 PageTables:        24916 kB\n"
        "Node 0 SecPageTables:         0 kB\n"
        "Node 0 NFS_Unstable:          0 kB\n"
        "Node 0 Bounce:                0 kB\n"
        "Node 0 WritebackTmp:          0 kB\n"
        "Node 0 KReclaimable:    1479140 kB\n"
        "Node 0 Slab:            1955976 kB\n"
        "Node 0 SReclaimable:    1479140 kB\n"
        "Node 0 SUnreclaim:       476836 kB\n"
        "Node 0 AnonHugePages:   3686400 kB\n"
        "Node 0 ShmemHugePages:        0 kB\n"
        "Node 0 ShmemPmdMapped:        0 kB\n"
        "Node 0 FileHugePages:        0 kB\n"
        "Node 0 FilePmdMapped:        0 kB\n"
        "Node 0 HugePages_Total:     0\n"
        "Node 0 HugePages_Free:      0\n"
        "Node 0 HugePages_Surp:      0\n";
    assert(parse(t) == (size_t)108191140 * 1024);
  }

  printf("regpool_node_avail: all cases passed\n");
  return 0;
}
