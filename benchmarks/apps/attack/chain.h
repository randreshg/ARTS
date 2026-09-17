/* chain.h -- closed-loop reader/writer chain engine for the attack programs.
 *
 * A program describes ONE workload in a spec: how many chains of each class
 * run on each rank, where the blocks are homed, how the chains couple, and
 * what each op touches.  The engine creates the chains from mainEdt, runs
 * them as self-perpetuating EDT sequences pinned to their rank, and prints the
 * program's result line once every chain has finished.
 *
 * Contract with the caller: mainEdt fills the spec and returns the result of
 * attack_chain_run(); nothing else is created by the program.  All per-chain
 * accounting travels in the chain's own paramv and lands in a per-chain
 * result block, so the engine shares no memory between EDTs. */
#ifndef ATTACK_CHAIN_H
#define ATTACK_CHAIN_H

#include "ocr.h"
#include "extensions/ocr-affinity.h"

#define ATTACK_CHAIN_MAX_BLOCKS 32u
#define ATTACK_CHAIN_MAX_CHAINS 1024u

typedef enum {
  ATTACK_CHAIN_SHARED,  /* every chain walks the shared block set */
  ATTACK_CHAIN_PRIVATE, /* every chain owns one block, homed one rank away
                         * (rank + 1); readers' blocks stay 0, writers' advance
                         * by exactly one per op */
} attack_chain_layout_t;

typedef struct {
  const char *name;            /* printed as "<NAME> OK ..." */
  attack_chain_layout_t layout;
  /* population, per ACTOR rank (the ranks that home no block); a program may
   * instead fix a class's total with *_total (then *_per_rank is 0) */
  u64 readers_per_rank, writers_per_rank;
  u64 readers_total, writers_total;
  u64 reader_ranks; /* 0 = every actor rank, else pack readers onto this many */
  u64 writer_ranks; /* 0 = every actor rank, else the lowest this many */
  u64 homes;        /* blocks are homed round-robin over ranks [0, homes);
                     * those ranks run no actor unless they are all the ranks */
  u64 blocks;       /* shared layout: block count (<= MAX_BLOCKS) */
  u64 bytes;
  u64 hold_r_us, hold_w_us, think_r_us, think_w_us;
  u64 ops_r, ops_w; /* exact ops per chain; a present class needs > 0 */
  int coupled;      /* chain c pinned to block c % blocks; a reader ends on
                     * having SEEN the stream's final counter value, so it
                     * cannot outrun its producers.  ops_r is therefore that
                     * final value -- the writers sharing the reader's block
                     * times ops_w -- and not the reader's own op count; a
                     * geometry that leaves a coupled reader's block without
                     * that many writes runs the reader out to the runaway
                     * cap and fails the cell (-CAP-HIT).  Shared layout only. */
  int first_touch_write; /* private layout: every chain's first op is an
                          * untimed write, so ownership starts on its rank */
  u64 writer_rings;      /* shared layout: 0 = a chain per writer slot; K >= 1 =
                          * the writer class runs as K rings instead, ring k
                          * cycling the contiguous slot block
                          * [k*(W/K), (k+1)*(W/K)), one chain of (W x ops_w / K)
                          * steps each.  A step takes the rank the ordinary
                          * placement rule gives its slot, and consecutive slots
                          * are consecutive ranks, so every step inside a block
                          * changes rank; the step that wraps a block goes back
                          * (W/K - 1) slots instead, so it changes rank unless
                          * the writer-rank count divides (W/K - 1) -- with two
                          * writer slots per rank and K == 2, only at a single
                          * writer rank.  A one-slot block (K == W) is the limit
                          * of that: nothing moves at all and the class is
                          * free-running writers by definition.  At most K writes
                          * are in flight, which is the longest chain of writers
                          * the home can queue.  K must divide W (and the class
                          * must be non-empty, shared layout).
                          * The class's per-op figures are NOT recorded, because
                          * a rotating chain's stamps come from a different clock
                          * at every step: its quantiles, means and throughput
                          * print -1 while the op counts and the value oracles
                          * stay exact. */
} attack_chain_spec_t;

/* Creates blocks, chains and the collector; returns NULL_GUID.  Prints
 * "<NAME>-ORACLE-FAIL kind=args" and shuts down on an impossible spec. */
ocrGuid_t attack_chain_run(const attack_chain_spec_t *spec, u64 pd_count);

#endif
