/**
 * stream_manual_arts.c -- Manual ARTS implementation of simplified STREAM
 *
 * Hand-written reference for what the CARTS compiler should generate.
 * Uses CPS chain pattern for the time-step loop with no blocking waits.
 *
 * STREAM kernels per iteration:
 *   Copy:  c[j] = a[j]              for j = 0..N-1
 *   Scale: b[j] = scalar * c[j]     for j = 0..N-1
 *   Add:   c[j] = a[j] + b[j]       for j = 0..N-1
 *   Triad: a[j] = b[j] + scalar*c[j] for j = 0..N-1
 *
 * EDT GRAPH (one iteration):
 *
 *   iter_edt (reads scratch via depv[0] RO)
 *     |
 *     |  Creates 4 kernel phases chained sequentially via epochs:
 *     |
 *     +-- copy_worker[0..P-1]    (parallel, inside copy_epoch)
 *     |     depv[0] = a_part[i] (RO), depv[1] = c_part[i] (EW)
 *     |
 *     +-- copy_done_edt          (copy_epoch finish -> launches scale)
 *     |     depv[0] = epoch signal, depv[1] = scratch (RO)
 *     |
 *     +-- scale_worker[0..P-1]   (parallel, inside scale_epoch)
 *     |     depv[0] = c_part[i] (RO), depv[1] = b_part[i] (EW)
 *     |
 *     +-- scale_done_edt         (scale_epoch finish -> launches add)
 *     |     depv[0] = epoch signal, depv[1] = scratch (RO)
 *     |
 *     +-- add_worker[0..P-1]     (parallel, inside add_epoch)
 *     |     depv[0] = a_part[i] (RO), depv[1] = b_part[i] (RO),
 *     |     depv[2] = c_part[i] (EW)
 *     |
 *     +-- add_done_edt           (add_epoch finish -> launches triad)
 *     |     depv[0] = epoch signal, depv[1] = scratch (RO)
 *     |
 *     +-- triad_worker[0..P-1]   (parallel, inside triad_epoch)
 *     |     depv[0] = b_part[i] (RO), depv[1] = c_part[i] (RO),
 *     |     depv[2] = a_part[i] (EW)
 *     |
 *     +-- triad_done_edt         (triad_epoch finish -> chains to advance)
 *     |     depv[0] = epoch signal, depv[1] = scratch (RO)
 *     |
 *     +-- advance_edt            (decides: next iteration or finish)
 *           depv[0] = new_scratch (RO)
 *
 * CPS CHAIN:
 *   advance_edt checks iter < NTIMES.  If yes, creates a new iter_edt with
 *   a fresh scratch DB.  If no, wires all data partitions + scratch to
 *   finish_edt for verification.
 *
 * DB FLOW:
 *   Each array (a, b, c) is partitioned into NUM_PARTITIONS separate DBs.
 *   Workers receive their partition(s) through depv slots.  Between kernels,
 *   the CDAG frontier ensures write-before-read ordering on the same DB GUIDs.
 *   The scratch DB carries partition GUIDs and iteration state across the
 *   CPS chain.
 *
 * BUILD:
 *   cc -O2 -o stream_manual_arts stream_manual_arts.c \
 *      -I$ARTS/include -L$ARTS/lib -larts -lpthread -lm
 *
 * RUN:
 *   ARTS_CONFIG=arts.cfg ./stream_manual_arts
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "arts.h"

/* ========================================================================= */
/* Configuration                                                             */
/* ========================================================================= */

#define N               1000
#define NTIMES          3
#define NUM_PARTITIONS  4
#define SCALAR          3.0

/* Partition size (elements per partition). N must be divisible by NUM_PARTITIONS. */
#define PART_SIZE  (N / NUM_PARTITIONS)

/* ========================================================================= */
/* Scratch DB layout -- carried through depv across the CPS chain            */
/* ========================================================================= */

typedef struct {
    uint32_t    iter;
    arts_guid_t finish_guid;
    arts_guid_t a_guids[NUM_PARTITIONS];
    arts_guid_t b_guids[NUM_PARTITIONS];
    arts_guid_t c_guids[NUM_PARTITIONS];
} scratch_t;

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */

void iter_edt(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]);
void copy_worker_edt(uint32_t paramc, const uint64_t *paramv,
                     uint32_t depc, arts_edt_dep_t depv[]);
void copy_done_edt(uint32_t paramc, const uint64_t *paramv,
                   uint32_t depc, arts_edt_dep_t depv[]);
void scale_worker_edt(uint32_t paramc, const uint64_t *paramv,
                      uint32_t depc, arts_edt_dep_t depv[]);
void scale_done_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]);
void add_worker_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]);
void add_done_edt(uint32_t paramc, const uint64_t *paramv,
                  uint32_t depc, arts_edt_dep_t depv[]);
void triad_worker_edt(uint32_t paramc, const uint64_t *paramv,
                      uint32_t depc, arts_edt_dep_t depv[]);
void triad_done_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]);
void advance_edt(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]);
void finish_edt(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]);

/* ========================================================================= */
/* Helper: create a scratch DB with current state                            */
/* ========================================================================= */

static arts_guid_t make_scratch(uint32_t iter, arts_guid_t finish_guid,
                                const arts_guid_t a_guids[],
                                const arts_guid_t b_guids[],
                                const arts_guid_t c_guids[]) {
    void *sp = NULL;
    arts_guid_t guid = arts_db_create(
        &sp, (uint64_t)sizeof(scratch_t), ARTS_DB_DEFAULT, NULL);
    scratch_t *s = (scratch_t *)sp;
    s->iter = iter;
    s->finish_guid = finish_guid;
    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        s->a_guids[i] = a_guids[i];
        s->b_guids[i] = b_guids[i];
        s->c_guids[i] = c_guids[i];
    }
    return guid;
}

/* ========================================================================= */
/* main_edt -- entry point on node 0                                         */
/*                                                                           */
/* Creates:                                                                  */
/*   - finish_edt (3*P+1 deps: a[0..P-1] RO, b[0..P-1] RO,                  */
/*                 c[0..P-1] RO, scratch RO)                                 */
/*   - 3 * NUM_PARTITIONS data DBs (a, b, c partitions)                      */
/*   - scratch DB with iteration 0 state                                     */
/*   - iter_edt to start iteration 0                                         */
/* ========================================================================= */

void main_edt(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc; (void)depv;

    arts_printf("[main] STREAM: N=%u, NTIMES=%u, PARTITIONS=%u, SCALAR=%.1f\n",
                N, NTIMES, NUM_PARTITIONS, SCALAR);

    /* finish_edt dep layout:
     *   slots 0..P-1     = a partitions (RO)
     *   slots P..2P-1    = b partitions (RO)
     *   slots 2P..3P-1   = c partitions (RO)
     *   slot  3P         = scratch (RO) -- not wired now, wired by advance
     *
     * Total deps = 3*NUM_PARTITIONS + 1
     * We leave all slots unsatisfied; advance_edt wires them at the end.
     */
    uint32_t finish_depc = 3 * NUM_PARTITIONS + 1;
    arts_guid_t fin_guid = arts_edt_create(
        finish_edt, 0, NULL, finish_depc, NULL);

    /* Create partition DBs for arrays a, b, c.
     * Initialize: a[j] = 1.0, b[j] = 2.0, c[j] = 0.0
     * (Standard STREAM initial values.) */
    arts_guid_t a_guids[NUM_PARTITIONS];
    arts_guid_t b_guids[NUM_PARTITIONS];
    arts_guid_t c_guids[NUM_PARTITIONS];

    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        void *ap = NULL, *bp = NULL, *cp = NULL;

        a_guids[i] = arts_db_create(
            &ap, (uint64_t)(PART_SIZE * sizeof(double)),
            ARTS_DB_DEFAULT, NULL);
        b_guids[i] = arts_db_create(
            &bp, (uint64_t)(PART_SIZE * sizeof(double)),
            ARTS_DB_DEFAULT, NULL);
        c_guids[i] = arts_db_create(
            &cp, (uint64_t)(PART_SIZE * sizeof(double)),
            ARTS_DB_DEFAULT, NULL);

        double *a = (double *)ap;
        double *b = (double *)bp;
        double *c = (double *)cp;
        for (unsigned int j = 0; j < PART_SIZE; j++) {
            a[j] = 1.0;
            b[j] = 2.0;
            c[j] = 0.0;
        }
    }

    /* Create scratch DB and launch iteration 0. */
    arts_guid_t scratch_guid = make_scratch(0, fin_guid, a_guids, b_guids, c_guids);

    arts_guid_t iter0 = arts_edt_create(iter_edt, 0, NULL, /*depc=*/1, NULL);
    arts_add_dependence(scratch_guid, iter0, 0, DB_MODE_RO);
}

/* ========================================================================= */
/* iter_edt -- one CPS iteration: launches Copy -> Scale -> Add -> Triad     */
/*                                                                           */
/* depv[0] = scratch DB (RO)                                                 */
/*                                                                           */
/* Strategy: chain 4 kernel phases sequentially using epochs.                */
/*   copy_epoch -> copy_done_edt -> (launches scale_epoch) -> ...            */
/* Each done_edt receives the scratch (RO) so it can read partition GUIDs    */
/* and wire the next kernel's workers.                                       */
/* ========================================================================= */

void iter_edt(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[0].ptr;
    uint32_t iter = scratch->iter;

    arts_printf("[iter %u] Starting STREAM kernels\n", iter);

    /* Create a fresh scratch copy to pass through the done-edt chain.
     * This avoids frontier conflicts: each EDT in the chain gets its own
     * RO acquire on a distinct scratch DB. */
    arts_guid_t chain_scratch = make_scratch(
        iter, scratch->finish_guid,
        scratch->a_guids, scratch->b_guids, scratch->c_guids);

    /* --- COPY kernel ---
     * copy_done_edt: 2 deps
     *   slot 0 = copy_epoch completion signal (value)
     *   slot 1 = chain_scratch (RO)
     */
    arts_guid_t copy_done = arts_edt_create(
        copy_done_edt, 0, NULL, /*depc=*/2, NULL);

    arts_guid_t copy_epoch = arts_initialize_and_start_epoch(
        copy_done, /*slot=*/0);

    /* Wire scratch to copy_done slot 1 */
    arts_add_dependence(chain_scratch, copy_done, 1, DB_MODE_RO);

    /* Launch copy workers (enrolled in copy_epoch).
     * Each worker: depv[0] = a_part[i] (RO), depv[1] = c_part[i] (EW)
     */
    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        arts_guid_t w = arts_edt_create_with_epoch(
            copy_worker_edt, 0, NULL, /*depc=*/2, copy_epoch, NULL);
        arts_add_dependence(scratch->a_guids[i], w, 0, DB_MODE_RO);
        arts_add_dependence(scratch->c_guids[i], w, 1, DB_MODE_EW);
    }

    /* When iter_edt returns, auto-acquire EW on chain_scratch is released.
     * Copy workers run in parallel.  When all finish, copy_epoch signals
     * copy_done at slot 0.  copy_done fires and launches scale. */
}

/* ========================================================================= */
/* COPY: c[j] = a[j]                                                        */
/* depv[0] = a_part (RO), depv[1] = c_part (EW)                             */
/* ========================================================================= */

void copy_worker_edt(uint32_t paramc, const uint64_t *paramv,
                     uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const double *a = (const double *)depv[0].ptr;
    double       *c = (double *)depv[1].ptr;

    for (unsigned int j = 0; j < PART_SIZE; j++)
        c[j] = a[j];
}

/* ========================================================================= */
/* copy_done_edt -- copy finished, launch SCALE kernel                       */
/*                                                                           */
/* depv[0] = epoch signal (value), depv[1] = scratch (RO)                    */
/* ========================================================================= */

void copy_done_edt(uint32_t paramc, const uint64_t *paramv,
                   uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[1].ptr;

    /* scale_done_edt: 2 deps (slot 0 = epoch signal, slot 1 = scratch RO) */
    arts_guid_t scale_done = arts_edt_create(
        scale_done_edt, 0, NULL, /*depc=*/2, NULL);

    arts_guid_t scale_epoch = arts_initialize_and_start_epoch(
        scale_done, /*slot=*/0);

    arts_add_dependence(depv[1].guid, scale_done, 1, DB_MODE_RO);

    /* Scale workers: depv[0] = c_part (RO), depv[1] = b_part (EW) */
    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        arts_guid_t w = arts_edt_create_with_epoch(
            scale_worker_edt, 0, NULL, /*depc=*/2, scale_epoch, NULL);
        arts_add_dependence(scratch->c_guids[i], w, 0, DB_MODE_RO);
        arts_add_dependence(scratch->b_guids[i], w, 1, DB_MODE_EW);
    }
}

/* ========================================================================= */
/* SCALE: b[j] = scalar * c[j]                                              */
/* depv[0] = c_part (RO), depv[1] = b_part (EW)                             */
/* ========================================================================= */

void scale_worker_edt(uint32_t paramc, const uint64_t *paramv,
                      uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const double *c = (const double *)depv[0].ptr;
    double       *b = (double *)depv[1].ptr;

    for (unsigned int j = 0; j < PART_SIZE; j++)
        b[j] = SCALAR * c[j];
}

/* ========================================================================= */
/* scale_done_edt -- scale finished, launch ADD kernel                       */
/*                                                                           */
/* depv[0] = epoch signal (value), depv[1] = scratch (RO)                    */
/* ========================================================================= */

void scale_done_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[1].ptr;

    /* add_done_edt: 2 deps (slot 0 = epoch signal, slot 1 = scratch RO) */
    arts_guid_t add_done = arts_edt_create(
        add_done_edt, 0, NULL, /*depc=*/2, NULL);

    arts_guid_t add_epoch = arts_initialize_and_start_epoch(
        add_done, /*slot=*/0);

    arts_add_dependence(depv[1].guid, add_done, 1, DB_MODE_RO);

    /* Add workers: depv[0] = a_part (RO), depv[1] = b_part (RO), depv[2] = c_part (EW) */
    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        arts_guid_t w = arts_edt_create_with_epoch(
            add_worker_edt, 0, NULL, /*depc=*/3, add_epoch, NULL);
        arts_add_dependence(scratch->a_guids[i], w, 0, DB_MODE_RO);
        arts_add_dependence(scratch->b_guids[i], w, 1, DB_MODE_RO);
        arts_add_dependence(scratch->c_guids[i], w, 2, DB_MODE_EW);
    }
}

/* ========================================================================= */
/* ADD: c[j] = a[j] + b[j]                                                  */
/* depv[0] = a_part (RO), depv[1] = b_part (RO), depv[2] = c_part (EW)      */
/* ========================================================================= */

void add_worker_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const double *a = (const double *)depv[0].ptr;
    const double *b = (const double *)depv[1].ptr;
    double       *c = (double *)depv[2].ptr;

    for (unsigned int j = 0; j < PART_SIZE; j++)
        c[j] = a[j] + b[j];
}

/* ========================================================================= */
/* add_done_edt -- add finished, launch TRIAD kernel                         */
/*                                                                           */
/* depv[0] = epoch signal (value), depv[1] = scratch (RO)                    */
/* ========================================================================= */

void add_done_edt(uint32_t paramc, const uint64_t *paramv,
                  uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[1].ptr;

    /* triad_done_edt: 2 deps (slot 0 = epoch signal, slot 1 = scratch RO) */
    arts_guid_t triad_done = arts_edt_create(
        triad_done_edt, 0, NULL, /*depc=*/2, NULL);

    arts_guid_t triad_epoch = arts_initialize_and_start_epoch(
        triad_done, /*slot=*/0);

    arts_add_dependence(depv[1].guid, triad_done, 1, DB_MODE_RO);

    /* Triad workers: depv[0] = b_part (RO), depv[1] = c_part (RO), depv[2] = a_part (EW) */
    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        arts_guid_t w = arts_edt_create_with_epoch(
            triad_worker_edt, 0, NULL, /*depc=*/3, triad_epoch, NULL);
        arts_add_dependence(scratch->b_guids[i], w, 0, DB_MODE_RO);
        arts_add_dependence(scratch->c_guids[i], w, 1, DB_MODE_RO);
        arts_add_dependence(scratch->a_guids[i], w, 2, DB_MODE_EW);
    }
}

/* ========================================================================= */
/* TRIAD: a[j] = b[j] + scalar * c[j]                                       */
/* depv[0] = b_part (RO), depv[1] = c_part (RO), depv[2] = a_part (EW)      */
/* ========================================================================= */

void triad_worker_edt(uint32_t paramc, const uint64_t *paramv,
                      uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const double *b = (const double *)depv[0].ptr;
    const double *c = (const double *)depv[1].ptr;
    double       *a = (double *)depv[2].ptr;

    for (unsigned int j = 0; j < PART_SIZE; j++)
        a[j] = b[j] + SCALAR * c[j];
}

/* ========================================================================= */
/* triad_done_edt -- triad finished, build next scratch and chain to advance */
/*                                                                           */
/* depv[0] = epoch signal (value), depv[1] = scratch (RO)                    */
/*                                                                           */
/* Creates a new scratch with iter+1 and wires it to advance_edt.            */
/* ========================================================================= */

void triad_done_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[1].ptr;

    /* Build new scratch for the advance decision.
     * iter is incremented here so advance sees the completed iteration count. */
    arts_guid_t new_scratch = make_scratch(
        scratch->iter + 1, scratch->finish_guid,
        scratch->a_guids, scratch->b_guids, scratch->c_guids);

    arts_guid_t adv = arts_edt_create(
        advance_edt, 0, NULL, /*depc=*/1, NULL);
    arts_add_dependence(new_scratch, adv, 0, DB_MODE_RO);
}

/* ========================================================================= */
/* advance_edt -- CPS decision: iterate again or finish                      */
/*                                                                           */
/* depv[0] = scratch (RO) with iter = completed iteration count              */
/*                                                                           */
/* If iter < NTIMES: create new iter_edt with fresh scratch.                 */
/* Otherwise: wire all data partitions to finish_edt for verification.       */
/* ========================================================================= */

void advance_edt(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[0].ptr;
    uint32_t iter = scratch->iter;

    arts_printf("[advance] Completed iteration %u / %u\n", iter, NTIMES);

    if (iter < NTIMES) {
        /* Chain to next iteration. */
        arts_guid_t next_scratch = make_scratch(
            iter, scratch->finish_guid,
            scratch->a_guids, scratch->b_guids, scratch->c_guids);

        arts_guid_t next_iter = arts_edt_create(
            iter_edt, 0, NULL, /*depc=*/1, NULL);
        arts_add_dependence(next_scratch, next_iter, 0, DB_MODE_RO);
    } else {
        /* All iterations complete.
         * Wire data partitions + scratch to finish_edt.
         *
         * finish_edt dep layout:
         *   slots 0..P-1     = a partitions (RO)
         *   slots P..2P-1    = b partitions (RO)
         *   slots 2P..3P-1   = c partitions (RO)
         *   slot  3P         = scratch (RO)
         */
        arts_guid_t fin = scratch->finish_guid;

        for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
            arts_add_dependence(scratch->a_guids[i], fin,
                                i, DB_MODE_RO);
            arts_add_dependence(scratch->b_guids[i], fin,
                                NUM_PARTITIONS + i, DB_MODE_RO);
            arts_add_dependence(scratch->c_guids[i], fin,
                                2 * NUM_PARTITIONS + i, DB_MODE_RO);
        }

        /* Wire a scratch copy to the last slot so finish can read metadata. */
        arts_guid_t result_scratch = make_scratch(
            iter, fin,
            scratch->a_guids, scratch->b_guids, scratch->c_guids);
        arts_add_dependence(result_scratch, fin,
                            3 * NUM_PARTITIONS, DB_MODE_RO);
    }
}

/* ========================================================================= */
/* finish_edt -- verify correctness and shutdown                             */
/*                                                                           */
/* Dep layout:                                                               */
/*   depv[0..P-1]     = a partitions (RO)                                    */
/*   depv[P..2P-1]    = b partitions (RO)                                    */
/*   depv[2P..3P-1]   = c partitions (RO)                                    */
/*   depv[3P]         = scratch (RO)                                         */
/*                                                                           */
/* After NTIMES iterations of (Copy, Scale, Add, Triad), the expected        */
/* values are determined by tracking the recurrence:                          */
/*   Init:  a=1, b=2, c=0                                                   */
/*   Copy:  c = a                                                            */
/*   Scale: b = scalar * c                                                   */
/*   Add:   c = a + b                                                        */
/*   Triad: a = b + scalar * c                                               */
/*                                                                           */
/* After 1 iteration:                                                        */
/*   c = 1        (Copy)                                                     */
/*   b = 3        (Scale: 3*1)                                               */
/*   c = 1+3 = 4  (Add)                                                      */
/*   a = 3+3*4 = 15 (Triad)                                                  */
/*                                                                           */
/* After 2 iterations:                                                       */
/*   c = 15        (Copy)                                                    */
/*   b = 45        (Scale: 3*15)                                             */
/*   c = 15+45 = 60 (Add)                                                    */
/*   a = 45+3*60 = 225 (Triad)                                               */
/*                                                                           */
/* After 3 iterations:                                                       */
/*   c = 225        (Copy)                                                   */
/*   b = 675        (Scale: 3*225)                                           */
/*   c = 225+675 = 900 (Add)                                                  */
/*   a = 675+3*900 = 3375 (Triad)                                            */
/* ========================================================================= */

void finish_edt(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    /* Compute expected values by replaying the recurrence. */
    double ea = 1.0, eb = 2.0, ec = 0.0;
    for (unsigned int k = 0; k < NTIMES; k++) {
        ec = ea;                        /* Copy */
        eb = SCALAR * ec;               /* Scale */
        ec = ea + eb;                   /* Add */
        ea = eb + SCALAR * ec;          /* Triad */
    }

    arts_printf("[finish] Expected: a=%.1f, b=%.1f, c=%.1f\n", ea, eb, ec);

    /* Verify each partition through depv. */
    double a_err = 0.0, b_err = 0.0, c_err = 0.0;
    for (unsigned int i = 0; i < NUM_PARTITIONS; i++) {
        const double *a_data = (const double *)depv[i].ptr;
        const double *b_data = (const double *)depv[NUM_PARTITIONS + i].ptr;
        const double *c_data = (const double *)depv[2 * NUM_PARTITIONS + i].ptr;

        for (unsigned int j = 0; j < PART_SIZE; j++) {
            a_err += fabs(a_data[j] - ea);
            b_err += fabs(b_data[j] - eb);
            c_err += fabs(c_data[j] - ec);
        }
    }

    arts_printf("[finish] Errors: a=%.6e, b=%.6e, c=%.6e\n", a_err, b_err, c_err);

    double total_err = a_err + b_err + c_err;
    if (total_err < 1e-6)
        arts_printf("[finish] PASSED\n");
    else
        arts_printf("[finish] FAILED (total error = %.6e)\n", total_err);

    arts_shutdown();
}

/* ========================================================================= */
/* main                                                                      */
/* ========================================================================= */

int main(int argc, char **argv) {
    arts_rt(argc, argv);
    return 0;
}
