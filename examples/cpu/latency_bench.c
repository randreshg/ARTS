/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

/*
 * latency_bench.c — CXL vs Local DRAM latency microbenchmark
 *
 * Measures per-access read and write latency for:
 *   LOCAL  : ARTS_DB_LOCAL  (node-local DRAM, no CDAG frontier)
 *   CXL    : ARTS_DB_CXL    (CXL-attached memory)
 *
 * Read latency is measured via pointer-chasing (serialized dependent loads),
 * which prevents hardware prefetching from masking true memory latency.
 *
 * Write latency is measured via sequential stores; the timing captures
 * effective write throughput per element.
 *
 * Buffer sizes sweep from 4 KB to 256 MB, covering the full cache hierarchy:
 *   4 KB – 64 KB   : L1/L2 cache — both tiers appear identical
 *   256 KB – 1 MB  : L3 cache boundary — divergence begins
 *   4 MB – 256 MB  : DRAM/CXL territory — full latency difference visible
 *
 * Usage:
 *   latency_bench [-n <ntimes>]
 *
 *   -n  Number of timing repetitions per (size, tier, op) tuple (default: 50)
 *
 * Output: tab-formatted table with Min/Avg/P50/P95/Max latency in ns/access.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "arts.h"
#include "arts/memory/db.h"
#include "arts/cxl/wrapper.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

#define NTIMES_DEFAULT  50

/* Number of pointer-chase steps per read timing rep.
 * Kept fixed across all buffer sizes so that each step causes a cache miss
 * whenever the buffer exceeds the LLC.  Capped at n_elems for small buffers. */
#define READ_CHAIN_LEN  4096ULL

/* Max elements to write per rep (caps wall-clock time for large buffers).
 * 65536 × 8 B = 512 KB — comfortably beyond L2 on most systems. */
#define WRITE_LEN_MAX   65536ULL

/* Sweep: 4 KB → 256 MB (9 sizes covering the full cache hierarchy) */
static const uint64_t SWEEP_SIZES[] = {
    4ULL   * 1024,
    16ULL  * 1024,
    64ULL  * 1024,
    256ULL * 1024,
    1ULL   * 1024 * 1024,
    4ULL   * 1024 * 1024,
    16ULL  * 1024 * 1024,
    64ULL  * 1024 * 1024,
    256ULL * 1024 * 1024,
};
#define NUM_SIZES ((uint32_t)(sizeof(SWEEP_SIZES) / sizeof(SWEEP_SIZES[0])))

/* Global EDT GUID array — needed for chaining in init_per_node */
static arts_guid_t bench_edts[9]; /* must match NUM_SIZES */

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int cmp_uint64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void fmt_size(uint64_t size, char *out, size_t outlen)
{
    if (size >= 1024ULL * 1024)
        snprintf(out, outlen, "%4lu MB", (unsigned long)(size >> 20));
    else
        snprintf(out, outlen, "%4lu KB", (unsigned long)(size >> 10));
}

/* Print one results row.
 * rep_totals[i] = total nanoseconds for rep i (NTIMES entries).
 * n_ops         = number of accesses per rep (for ns/access calculation). */
static void print_row(uint64_t size, const char *tier, const char *op,
                      uint64_t *rep_totals, uint32_t n, uint64_t n_ops)
{
    qsort(rep_totals, n, sizeof(uint64_t), cmp_uint64);

    double total_sum = 0.0;
    for (uint32_t i = 0; i < n; i++)
        total_sum += (double)rep_totals[i];

    double scale = 1.0 / (double)n_ops;
    double avg   = (total_sum / (double)n) * scale;
    double min   = (double)rep_totals[0]                         * scale;
    double max   = (double)rep_totals[n - 1]                     * scale;
    double p50   = (double)rep_totals[n / 2]                     * scale;
    double p95   = (double)rep_totals[(uint32_t)(n * 0.95f)]     * scale;
    double p99   = (double)rep_totals[(uint32_t)(n * 0.99f)]     * scale;

    char size_str[12];
    fmt_size(size, size_str, sizeof(size_str));

    arts_printf("%9s | %-5s | %-5s | %8.2f | %8.2f | %8.2f | %8.2f | %8.2f | %8.2f\n",
                size_str, tier, op, min, avg, p50, p95, p99, max);
}

/* Fisher-Yates shuffle: builds a pointer-chase permutation in buf[0..n-1].
 * buf[i] holds the next index; following the chain visits each element
 * exactly once in pseudo-random order (defeats hardware prefetchers). */
static void build_permutation(uint64_t *buf, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        buf[i] = i;

    /* xorshift64 for fast, seed-based pseudo-random permutation */
    uint64_t rng = 0xdeadbeefcafe1234ULL;
    for (uint64_t i = n - 1; i > 0; i--) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        uint64_t j = rng % (i + 1);
        uint64_t tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
    }
}

/* =========================================================================
 * bench_edt — measure read + write latency for one buffer size
 *
 * paramv[0] = ntimes
 * paramv[1] = size in bytes
 * paramv[2] = next_edt_guid  (NULL_GUID for last size → triggers shutdown)
 * paramv[3] = (uint64_t) local_buf pointer
 * paramv[4] = (uint64_t) cxl_buf pointer
 * paramv[5] = cxl_guid
 * ========================================================================= */
void bench_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[])
{
    uint32_t      ntimes    = (uint32_t)    paramv[0];
    uint64_t      size      =               paramv[1];
    arts_guid_t   next      = (arts_guid_t) paramv[2];
    uint64_t     *local_buf = (uint64_t *)  paramv[3];
    uint64_t     *cxl_buf   = (uint64_t *)  paramv[4];
    arts_guid_t   cxl_guid  = (arts_guid_t) paramv[5];

    uint64_t n_elems     = size / sizeof(uint64_t);
    uint64_t chain_len   = (n_elems < READ_CHAIN_LEN) ? n_elems : READ_CHAIN_LEN;
    uint64_t write_len   = (n_elems < WRITE_LEN_MAX)  ? n_elems : WRITE_LEN_MAX;

    uint64_t *read_local  = malloc(ntimes * sizeof(uint64_t));
    uint64_t *read_cxl    = malloc(ntimes * sizeof(uint64_t));
    uint64_t *write_local = malloc(ntimes * sizeof(uint64_t));
    uint64_t *write_cxl   = malloc(ntimes * sizeof(uint64_t));

    /* ------------------------------------------------------------------
     * READ PHASE — pointer-chase (read phase must precede write phase
     * because writes overwrite the pointer-chase permutation)
     * ------------------------------------------------------------------ */

    /* Local DRAM reads */
    for (uint32_t rep = 0; rep < ntimes; rep++) {
        volatile uint64_t cur = 0;
        uint64_t t0 = arts_get_time_stamp();
        for (uint64_t step = 0; step < chain_len; step++)
            cur = local_buf[cur];
        read_local[rep] = arts_get_time_stamp() - t0;
        (void)cur; /* prevent dead-code elimination */
    }

    /* CXL reads — flush before first rep to ensure coherent view */
    arts_cxl_consumer_flush(cxl_guid);
    for (uint32_t rep = 0; rep < ntimes; rep++) {
        volatile uint64_t cur = 0;
        uint64_t t0 = arts_get_time_stamp();
        arts_cxl_consumer_flush(cxl_guid);
        for (uint64_t step = 0; step < chain_len; step++)
            cur = cxl_buf[cur];
        read_cxl[rep] = arts_get_time_stamp() - t0;
        (void)cur;
    }

    /* ------------------------------------------------------------------
     * WRITE PHASE — sequential stores (write_len elements)
     * ------------------------------------------------------------------ */

    /* Local DRAM writes */
    for (uint32_t rep = 0; rep < ntimes; rep++) {
        uint64_t t0 = arts_get_time_stamp();
        for (uint64_t i = 0; i < write_len; i++)
            local_buf[i] = i;
        write_local[rep] = arts_get_time_stamp() - t0;
    }

    /* CXL writes — flush after each rep to measure full write completion */
    for (uint32_t rep = 0; rep < ntimes; rep++) {
        uint64_t t0 = arts_get_time_stamp();
        for (uint64_t i = 0; i < write_len; i++)
            cxl_buf[i] = i;
        arts_cxl_producer_flush(cxl_guid);
        write_cxl[rep] = arts_get_time_stamp() - t0;
    }

    /* ------------------------------------------------------------------
     * Print results
     * ------------------------------------------------------------------ */
    print_row(size, "LOCAL", "read",  read_local,  ntimes, chain_len);
    print_row(size, "CXL",   "read",  read_cxl,    ntimes, chain_len);
    print_row(size, "LOCAL", "write", write_local, ntimes, write_len);
    print_row(size, "CXL",   "write", write_cxl,   ntimes, write_len);
    arts_printf("----------|-------|-------|----------|----------|----------|----------|----------|----------\n");

    free(read_local);
    free(read_cxl);
    free(write_local);
    free(write_cxl);

    /* Chain to next size or shut down */
    if (next != NULL_GUID)
        arts_signal_edt_null(next, 0);
    else
        arts_shutdown();
}

/* =========================================================================
 * init_per_node — allocate buffers and wire up the EDT chain
 * ========================================================================= */
void init_per_node(unsigned int node_id, int argc, char **argv)
{
    if (node_id)
        return;

    uint32_t ntimes = NTIMES_DEFAULT;
    int opt;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        if (opt == 'n')
            ntimes = (uint32_t)strtoul(optarg, NULL, 10);
    }
    if (ntimes == 0)
        ntimes = NTIMES_DEFAULT;

    /* Reserve all bench_edt GUIDs upfront (needed for building next-pointers) */
    for (uint32_t i = 0; i < NUM_SIZES; i++)
        bench_edts[i] = arts_guid_reserve(ARTS_EDT, 0);

    /* Allocate + initialise buffers, then create bench EDTs */
    for (uint32_t i = 0; i < NUM_SIZES; i++) {
        uint64_t size    = SWEEP_SIZES[i];
        uint64_t n_elems = size / sizeof(uint64_t);

        /* Local DRAM buffer — ARTS_DB_LOCAL: node-pinned, no CDAG frontier */
        uint64_t *local_ptr;
        arts_db_create((void **)&local_ptr, size, ARTS_DB_LOCAL, NULL);
        build_permutation(local_ptr, n_elems);

        /* CXL buffer — same permutation so read phases are comparable */
        uint64_t *cxl_ptr;
        arts_guid_t cxl_guid = arts_db_create((void **)&cxl_ptr, size,
                                               ARTS_DB_CXL, NULL);
        memcpy(cxl_ptr, local_ptr, size);
        arts_cxl_producer_flush(cxl_guid);

        arts_guid_t next = (i + 1 < NUM_SIZES) ? bench_edts[i + 1] : NULL_GUID;

        uint64_t params[6] = {
            (uint64_t)ntimes,
            (uint64_t)size,
            (uint64_t)next,
            (uint64_t)local_ptr,
            (uint64_t)cxl_ptr,
            (uint64_t)cxl_guid,
        };
        arts_edt_create_with_guid(bench_edt, bench_edts[i], 6, params, 1);
    }
}

/* =========================================================================
 * init_per_worker — print header and kick off the first bench_edt
 * ========================================================================= */
void init_per_worker(unsigned int node_id, unsigned int worker_id,
                     int argc, char **argv)
{
    if (node_id || worker_id)
        return;

    /* Write-back/invalidate caches to ensure cold-cache first measurement */
    wbinv();

    arts_printf("\n=== CXL vs Local DRAM Latency Profile ===\n");
    arts_printf("READ: pointer-chase (%llu steps/rep)  "
                "WRITE: sequential stores (max %llu elements)\n\n",
                (unsigned long long)READ_CHAIN_LEN,
                (unsigned long long)WRITE_LEN_MAX);
    arts_printf("%9s | %-5s | %-5s | %8s | %8s | %8s | %8s | %8s | %8s\n",
                "Size", "Tier", "Op",
                "Min(ns)", "Avg(ns)", "P50(ns)", "P95(ns)", "P99(ns)", "Max(ns)");
    arts_printf("----------|-------|-------|----------|----------|----------|----------|----------|----------\n");

    arts_signal_edt_null(bench_edts[0], 0);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char **argv)
{
    arts_rt(argc, argv);
    return 0;
}
