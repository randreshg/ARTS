/**
 * cps_chain.c -- CPS-chain pattern for ARTS (distributed-safe)
 *
 * Demonstrates continuation-passing style (CPS) iteration over epochs
 * with parallel workers.  All data flows through DB dependencies (depv),
 * never through raw pointers in paramv.
 *
 * DESIGN RULES:
 *
 *   1. DATA THROUGH DEPV, NOT PARAMV.
 *      Workers receive data DBs through depv[slot].ptr, resolved by the
 *      runtime to a valid local pointer on any node.  Only GUIDs (uint64_t)
 *      go into paramv.
 *
 *   2. SEPARATE DBs PER WORKER (no frontier serialization).
 *      Each worker i writes to its own new_partition[i] DB.  Distinct GUIDs
 *      mean distinct CDAG frontier entries, so N workers run in parallel.
 *      Sharing one DB with DB_MODE_EW would serialize them.
 *
 *   3. SCRATCH DB FOR SEQUENTIAL STATE.
 *      The continuation needs mutable iteration state (iter counter, norm,
 *      partition GUID array).  This lives in a scratch DB delivered via depv.
 *      A fresh scratch is created each iteration to avoid frontier conflicts.
 *
 *   4. NO BLOCKING (no arts_wait_on_handle).
 *      Each iteration is fully asynchronous:
 *        outer_epoch -> inner_epoch -> N workers -> cont_edt -> advance_edt
 *
 *   5. CDAG FRONTIER ORDERING.
 *      chain_iter_edt creates the scratch DB (holds auto-acquire EW), then
 *      wires it to cont_edt (EW) and advance_edt (RO).  The frontier
 *      ensures advance sees the scratch only after cont releases its EW:
 *        iter_edt(EW-create) -> cont_edt(EW) -> advance_edt(RO)
 *
 * EDT GRAPH (one iteration):
 *
 *   chain_iter_edt
 *     |  creates outer epoch, inner epoch, workers, cont, advance
 *     |
 *     +-- worker_edt[0] \                    (inside inner epoch)
 *     +-- worker_edt[1]  |-- all parallel
 *     +-- ...            |
 *     +-- worker_edt[N] /
 *     |
 *     +-- chain_cont_edt     (inner epoch finish -> sequential norm compute)
 *     |     writes norm into scratch DB
 *     |
 *     +-- chain_advance_edt  (outer epoch finish -> decide next iter or done)
 *           reads scratch DB (after cont releases EW)
 *           if more iters: creates new chain_iter_edt
 *           else: signals finish_edt
 *
 * BUILD:
 *   cc -O2 -o cps_chain cps_chain.c -I$ARTS/include -L$ARTS/lib -larts -lpthread -lm
 *
 * RUN:
 *   ARTS_CONFIG=arts.cfg ./cps_chain
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "arts.h"

/* ========================================================================= */
/* Configuration                                                             */
/* ========================================================================= */

#define NUM_WORKERS     4
#define PARTITION_SIZE  256
#define MAX_ITER        10
#define DECAY_FACTOR    0.5

/* ========================================================================= */
/* Scratch DB layout -- carried through depv, portable across nodes          */
/* ========================================================================= */

typedef struct {
    uint32_t    iter;
    uint32_t    max_iter;
    uint32_t    num_workers;
    uint32_t    partition_size;
    double      norm;
    arts_guid_t finish_guid;
    arts_guid_t partition_guids[NUM_WORKERS];
} scratch_t;

/* ========================================================================= */
/* Forward declarations                                                      */
/* ========================================================================= */

void chain_iter_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]);
void worker_edt(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]);
void chain_cont_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]);
void chain_advance_edt(uint32_t paramc, const uint64_t *paramv,
                       uint32_t depc, arts_edt_dep_t depv[]);
void finish_edt(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]);

/* ========================================================================= */
/* main_edt -- entry point on node 0.  Creates finish EDT, initial           */
/*             partition DBs, scratch DB, and launches iteration 0.          */
/*             Returns immediately (no blocking).                            */
/* ========================================================================= */

void main_edt(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc; (void)depv;

    arts_printf("[main] CPS chain: %u workers, %u iters, %u doubles/partition\n",
                NUM_WORKERS, MAX_ITER, PARTITION_SIZE);

    /* -- finish_edt: 1 dep slot (slot 0 = result scratch DB, RO) -- */
    arts_guid_t finish_guid = arts_edt_create(
        finish_edt, 0, NULL, /*depc=*/1, NULL);

    /* -- Create NUM_WORKERS partition DBs on the current node --
     *
     * All created locally so we can initialize them.  The runtime migrates
     * data to remote nodes when workers acquire them through depv.
     * Each partition is a separate GUID so workers don't serialize.
     */
    arts_guid_t part_guids[NUM_WORKERS];
    for (unsigned int i = 0; i < NUM_WORKERS; i++) {
        void *ptr = NULL;
        part_guids[i] = arts_db_create(
            &ptr,
            (uint64_t)(PARTITION_SIZE * sizeof(double)),
            ARTS_DB_DEFAULT,
            NULL);
        double *data = (double *)ptr;
        for (unsigned int j = 0; j < PARTITION_SIZE; j++) {
            data[j] = (double)(i * PARTITION_SIZE + j + 1);
        }
    }

    /* -- Create scratch DB with iteration 0 metadata -- */
    void *sp = NULL;
    arts_guid_t scratch_guid = arts_db_create(
        &sp, (uint64_t)sizeof(scratch_t), ARTS_DB_DEFAULT, NULL);
    scratch_t *scratch    = (scratch_t *)sp;
    scratch->iter           = 0;
    scratch->max_iter       = MAX_ITER;
    scratch->num_workers    = NUM_WORKERS;
    scratch->partition_size = PARTITION_SIZE;
    scratch->norm           = 0.0;
    scratch->finish_guid    = finish_guid;
    for (unsigned int i = 0; i < NUM_WORKERS; i++)
        scratch->partition_guids[i] = part_guids[i];

    /* -- Launch iteration 0 --
     *
     * chain_iter_edt has 1 dep: slot 0 = scratch (RO).
     * arts_add_dependence wires the scratch DB to the EDT.  When the DB is
     * ready (frontier allows RO after the auto-acquire EW from creation is
     * released when main_edt returns), the EDT fires.
     */
    arts_guid_t iter_guid = arts_edt_create(
        chain_iter_edt, 0, NULL, /*depc=*/1, NULL);
    arts_add_dependence(scratch_guid, iter_guid, 0, DB_MODE_RO);
}

/* ========================================================================= */
/* chain_iter_edt -- one CPS iteration                                       */
/*                                                                           */
/* depv[0] = scratch DB (RO) with iteration state and partition GUIDs        */
/*                                                                           */
/* Creates:                                                                  */
/*   - N new output partition DBs                                            */
/*   - A new scratch DB (for cont to write norm into)                        */
/*   - advance_edt (outer epoch finish target), 2 deps:                      */
/*       slot 0 = outer epoch completion signal (value from runtime)         */
/*       slot 1 = new scratch DB (RO, after cont releases EW)               */
/*   - outer epoch -> advance_edt slot 0                                     */
/*   - cont_edt (inner epoch finish target), N+2 deps:                       */
/*       slots 0..N-1 = new partitions (RO)                                 */
/*       slot  N      = new scratch DB (EW, cont writes norm)               */
/*       slot  N+1    = inner epoch completion signal (value)                */
/*   - inner epoch (nested in outer) -> cont_edt slot N+1                   */
/*   - N worker EDTs (in inner epoch), each 2 deps:                          */
/*       slot 0 = old partition (RO)                                         */
/*       slot 1 = new partition (EW)                                         */
/*                                                                           */
/* CDAG frontier ordering for the scratch DB:                                */
/*   iter_edt auto-acquire(EW) -> cont_edt(EW) -> advance_edt(RO)          */
/*   This guarantees advance sees the norm after cont writes it.             */
/* ========================================================================= */

void chain_iter_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[0].ptr;
    uint32_t iter        = scratch->iter;
    uint32_t num_w       = scratch->num_workers;
    uint32_t part_size   = scratch->partition_size;
    arts_guid_t fin_guid = scratch->finish_guid;
    unsigned int nodes   = arts_get_total_nodes();

    arts_printf("[iter %u] Launching %u workers\n", iter, num_w);

    /* -- New output partition DBs (one per worker, separate GUIDs) -- */
    arts_guid_t new_parts[NUM_WORKERS];
    for (unsigned int i = 0; i < num_w; i++) {
        void *p = NULL;
        new_parts[i] = arts_db_create(
            &p, (uint64_t)(part_size * sizeof(double)),
            ARTS_DB_DEFAULT, NULL);
        memset(p, 0, part_size * sizeof(double));
    }

    /* -- New scratch DB (iter_edt holds auto-acquire EW on creation) -- */
    void *sp = NULL;
    arts_guid_t new_scratch_guid = arts_db_create(
        &sp, (uint64_t)sizeof(scratch_t), ARTS_DB_DEFAULT, NULL);
    scratch_t *ns   = (scratch_t *)sp;
    ns->iter           = iter;
    ns->max_iter       = scratch->max_iter;
    ns->num_workers    = num_w;
    ns->partition_size = part_size;
    ns->norm           = 0.0;
    ns->finish_guid    = fin_guid;
    for (unsigned int i = 0; i < num_w; i++)
        ns->partition_guids[i] = new_parts[i];

    /* -- advance_edt: outer epoch finish target --
     *    slot 0 = outer epoch signal (value)
     *    slot 1 = new scratch (RO, resolves after cont releases EW)
     */
    arts_guid_t advance_guid = arts_edt_create(
        chain_advance_edt, 0, NULL, /*depc=*/2, NULL);

    /* -- Outer epoch: signals advance_edt slot 0 on completion -- */
    arts_guid_t outer_epoch = arts_initialize_and_start_epoch(
        advance_guid, /*slot=*/0);

    /* -- cont_edt: inner epoch finish target (enrolled in outer epoch) --
     *    slots 0..N-1 = new partitions (RO)
     *    slot  N      = new scratch (EW, cont writes norm)
     *    slot  N+1    = inner epoch signal (value)
     */
    uint32_t cont_depc = num_w + 2;
    arts_guid_t cont_guid = arts_edt_create_with_epoch(
        chain_cont_edt, 0, NULL, cont_depc, outer_epoch, NULL);

    /* -- Inner epoch: signals cont_edt slot N+1 on completion -- */
    arts_guid_t inner_epoch = arts_initialize_and_start_epoch(
        cont_guid, /*slot=*/num_w + 1);

    /* -- N worker EDTs (enrolled in inner epoch) --
     *    slot 0 = old partition (RO)
     *    slot 1 = new partition (EW)
     *    paramv[0] = worker index
     *
     * Separate GUIDs per worker -> no frontier serialization.
     *
     * IMPORTANT: Wire workers' EW on new_parts[i] BEFORE cont's RO on
     * the same DBs.  The CDAG frontier orders acquire requests in the
     * sequence they are registered.  We need:
     *   iter_edt(create-EW) -> worker[i](EW) -> cont(RO)
     * for each new_parts[i].
     */
    for (unsigned int i = 0; i < num_w; i++) {
        uint64_t wp[1] = { (uint64_t)i };
        unsigned int route = i % nodes;
        arts_guid_t w = arts_edt_create_with_epoch(
            worker_edt, 1, wp, /*depc=*/2, inner_epoch,
            &(arts_hint_t){ .route = route });

        arts_add_dependence(scratch->partition_guids[i], w, 0, DB_MODE_RO);
        arts_add_dependence(new_parts[i], w, 1, DB_MODE_EW);
    }

    /* Now wire cont's RO deps on new_parts[i] AFTER all workers' EW deps.
     * Frontier: worker[i](EW) completes before cont(RO) resolves. */
    for (unsigned int i = 0; i < num_w; i++)
        arts_add_dependence(new_parts[i], cont_guid, i, DB_MODE_RO);

    /* Wire scratch to cont (EW) and advance (RO).
     *
     * Frontier ordering for scratch DB:
     *   iter_edt(create-EW) -> cont_edt(EW) -> advance_edt(RO)
     *
     * Wire cont's EW first, then advance's RO, to get correct ordering.
     */
    arts_add_dependence(new_scratch_guid, cont_guid, num_w, DB_MODE_EW);
    arts_add_dependence(new_scratch_guid, advance_guid, 1, DB_MODE_RO);
    /* cont slot N+1: filled by inner epoch completion signal (automatic) */

    /* iter_edt returns -> releases auto-acquire EW on new_scratch_guid
     * and all new_parts[i].
     * Workers' EW deps resolve first (registered before cont's RO).
     * After all workers finish, inner epoch signals cont at slot N+1.
     * cont gets EW on scratch + RO on partitions, writes norm, returns.
     * advance gets RO on scratch (with norm) + outer epoch signal. */
}

/* ========================================================================= */
/* worker_edt -- process one partition (inside inner epoch)                   */
/*                                                                           */
/* paramv[0] = worker index                                                  */
/* depv[0]   = old partition (RO)                                            */
/* depv[1]   = new partition (EW)                                            */
/*                                                                           */
/* Each worker has its own distinct DB pair -> runs in parallel with others. */
/* ========================================================================= */

void worker_edt(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)depc;

    const double *old_data = (const double *)depv[0].ptr;
    double       *new_data = (double *)depv[1].ptr;

    /* Decay transform: new[j] = old[j] * DECAY_FACTOR.
     * Simulates an iterative computation (e.g., Jacobi relaxation). */
    for (unsigned int j = 0; j < PARTITION_SIZE; j++)
        new_data[j] = old_data[j] * DECAY_FACTOR;

    /* On return: EW on new partition released, inner epoch decremented. */
    (void)paramv;
}

/* ========================================================================= */
/* chain_cont_edt -- sequential work after all workers finish                */
/*                                                                           */
/* depv[0..N-1] = new partitions (RO)                                        */
/* depv[N]      = scratch DB (EW) -- we write the computed norm into it      */
/* depv[N+1]    = inner epoch completion signal (value)                       */
/*                                                                           */
/* After computing norm and writing it to scratch, this EDT returns,          */
/* releasing the EW on scratch.  The CDAG frontier then allows               */
/* advance_edt's RO on the same scratch to resolve.                          */
/* ========================================================================= */

void chain_cont_edt(uint32_t paramc, const uint64_t *paramv,
                    uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    scratch_t *scratch = (scratch_t *)depv[NUM_WORKERS].ptr;
    uint32_t iter      = scratch->iter;
    uint32_t part_size = scratch->partition_size;

    /* Compute L2 norm across all new partitions. */
    double sum_sq = 0.0;
    for (unsigned int i = 0; i < NUM_WORKERS; i++) {
        const double *data = (const double *)depv[i].ptr;
        for (unsigned int j = 0; j < part_size; j++)
            sum_sq += data[j] * data[j];
    }
    scratch->norm = sqrt(sum_sq);

    arts_printf("[cont  iter %u] L2 norm = %.6f\n", iter, scratch->norm);

    /* On return: EW on scratch released -> advance_edt's RO resolves. */
}

/* ========================================================================= */
/* chain_advance_edt -- decide: iterate again or finish                      */
/*                                                                           */
/* depv[0] = outer epoch completion signal (value)                           */
/* depv[1] = scratch DB (RO) -- contains updated norm from cont_edt          */
/*                                                                           */
/* If iter+1 < max_iter: create new scratch, new chain_iter_edt.             */
/* Otherwise: wire result to finish_edt -> shutdown.                         */
/* ========================================================================= */

void chain_advance_edt(uint32_t paramc, const uint64_t *paramv,
                       uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *scratch = (const scratch_t *)depv[1].ptr;
    uint32_t iter        = scratch->iter;
    uint32_t max_iter    = scratch->max_iter;
    uint32_t num_w       = scratch->num_workers;
    uint32_t part_size   = scratch->partition_size;
    double   norm        = scratch->norm;
    arts_guid_t fin_guid = scratch->finish_guid;

    arts_printf("[advance iter %u] norm=%.6f\n", iter, norm);

    uint32_t next_iter = iter + 1;

    if (next_iter < max_iter) {
        /* -- Chain to next iteration -- */
        void *sp = NULL;
        arts_guid_t next_scratch = arts_db_create(
            &sp, (uint64_t)sizeof(scratch_t), ARTS_DB_DEFAULT, NULL);
        scratch_t *ns    = (scratch_t *)sp;
        ns->iter           = next_iter;
        ns->max_iter       = max_iter;
        ns->num_workers    = num_w;
        ns->partition_size = part_size;
        ns->norm           = 0.0;
        ns->finish_guid    = fin_guid;
        /* Carry forward the NEW partition GUIDs (output of this iteration). */
        for (unsigned int i = 0; i < num_w; i++)
            ns->partition_guids[i] = scratch->partition_guids[i];

        arts_guid_t next_iter_guid = arts_edt_create(
            chain_iter_edt, 0, NULL, /*depc=*/1, NULL);
        arts_add_dependence(next_scratch, next_iter_guid, 0, DB_MODE_RO);

        arts_printf("[advance] -> iteration %u\n", next_iter);
    } else {
        /* -- Chain complete: deliver result to finish_edt -- */
        arts_printf("[advance] Chain complete (%u iters), final norm=%.6f\n",
                    max_iter, norm);

        void *rp = NULL;
        arts_guid_t result_guid = arts_db_create(
            &rp, (uint64_t)sizeof(scratch_t), ARTS_DB_DEFAULT, NULL);
        memcpy(rp, scratch, sizeof(scratch_t));

        arts_add_dependence(result_guid, fin_guid, 0, DB_MODE_RO);
    }
}

/* ========================================================================= */
/* finish_edt -- verify and shutdown                                         */
/*                                                                           */
/* depv[0] = result scratch DB (RO)                                          */
/* ========================================================================= */

void finish_edt(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]) {
    (void)paramc; (void)paramv; (void)depc;

    const scratch_t *r = (const scratch_t *)depv[0].ptr;

    arts_printf("[finish] Completed %u / %u iterations\n",
                r->iter + 1, r->max_iter);
    arts_printf("[finish] Final L2 norm: %.6f\n", r->norm);

    /* Expected: each element v starts as v = 1..N, after MAX_ITER iters
     * of multiplying by DECAY_FACTOR: v * DECAY^MAX_ITER.
     * L2 norm = DECAY^MAX_ITER * sqrt(sum_{k=1}^{N} k^2)
     *         = DECAY^MAX_ITER * sqrt(N*(N+1)*(2N+1)/6)
     */
    uint32_t N = r->num_workers * r->partition_size;
    double decay = pow(DECAY_FACTOR, (double)r->max_iter);
    double expected = decay * sqrt((double)N * (N + 1.0) * (2.0 * N + 1.0) / 6.0);

    double rel_err = fabs(r->norm - expected) / expected;
    arts_printf("[finish] Expected norm: %.6f, relative error: %.2e\n",
                expected, rel_err);

    if (rel_err < 1e-10)
        arts_printf("[finish] PASSED\n");
    else
        arts_printf("[finish] FAILED\n");

    arts_shutdown();
}

/* ========================================================================= */
/* main                                                                      */
/* ========================================================================= */

int main(int argc, char **argv) {
    arts_rt(argc, argv);
    return 0;
}
