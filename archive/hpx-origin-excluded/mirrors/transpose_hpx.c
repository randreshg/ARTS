/* OCR mirror of HPX's transpose_block: the PRK matrix transpose B = A^T over
 * column blocks.  Column block `b` is `num_blocks` square sub-blocks of
 * `block_order` doubles a side, all homed at b's owner; one task per
 * (block, phase) per iteration transposes one square out of the phase's
 * column block into the block's own.  A rank joins its own blocks' tasks
 * each iteration, as the origin's per-locality wait does, and rank 0
 * accumulates the squared error over its own blocks between that join and
 * the next iteration. */
#include "hpx_mirror.h"
#include <float.h>

enum { P_ITERS, P_TILE, P_NLB, P_NB, P_BO, P_NL, P_RANK, P_ITER, P_BLOCK,
       P_RANGE, P_MAP, P_ACC, P_FILL_TPL, P_PUBLISH_TPL, P_XPOSE_TPL,
       P_SPAWN_TPL, P_BLOCKSPAWN_TPL, P_BLOCKJOIN_TPL, P_CHECK_TPL,
       P_REDUCE_TPL, P_DONE_TPL, P_ELAPSED_TPL, P_SHUTDOWN_TPL,
       P_JOIN, P_START_NS, P_ITER_START, P_VERBOSE, P_COUNT };

typedef struct {
  double error, average, minimum, maximum;
} measurements_t;

/* The origin's index shifts, and the threshold it validates against. */
#define COL_SHIFT 1000.00
#define ROW_SHIFT 0.001
#define EPSILON 1.e-8

/* Two rendezvous points, and everything else a task reads is already written
 * when that task is created.  A table carries an owner's sub-block names to
 * a consumer that has no way to learn them; a completion signal carries a
 * rank's last turn to the rank that ends the program.  An ordinal names one
 * of these for one rank and is never reused, and the kind stride leaves no
 * aliasing between adjacent ranks. */
enum { KIND_TABLE, KIND_DONE, KIND_COUNT };

static inline ocrGuid_t point(u64 *pv, u64 index, u64 kind, u64 consumer) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]), index * KIND_COUNT + kind,
                     consumer, pv[P_NL]);
}

/* One column block's squares, created and filled by the task that owns it:
 * the origin fills a locality's blocks with one parallel task per block, and
 * this phase is inside the measured window on both sides. */
static ocrGuid_t fill_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nb = pv[P_NB], nlb = pv[P_NLB], bo = pv[P_BO], rank = pv[P_RANK];
  u64 x = rank * nlb + pv[P_BLOCK], bs = bo * bo;
  ocrGuid_t *names = depv[0].ptr, *bnames = names + nb;

  ocrHint_t dh;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  for (u64 y = 0; y < nb; ++y) {
    double *a, *b;
    ocrGuid_t adb, bdb;
    ocrDbCreate(&adb, (void **)&a, bs * sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
    ocrDbCreate(&bdb, (void **)&b, bs * sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
    memset(a, 0, bs * sizeof(double));
    memset(b, 0, bs * sizeof(double));
    /* A is the matrix the origin fills, B its known garbage value; a square
     * holds the rows of one band and the columns of one block. */
    for (u64 r = 0; r < bo; ++r) {
      for (u64 c = 0; c < bo; ++c) {
        double col_val = COL_SHIFT * (double)(x * bo + c);
        a[r * bo + c] = col_val + ROW_SHIFT * (double)(y * bo + r);
        b[r * bo + c] = -1.0;
      }
    }
    ocrDbRelease(adb);
    ocrDbRelease(bdb);
    names[y] = adb;
    bnames[y] = bdb;
  }
  return NULL_GUID;
}

/* The rank's names, once its blocks exist: its A squares to every rank, its
 * B squares to itself.  The table leaves released and then travels, and the
 * map is released before the task that reads it is given it. */
static ocrGuid_t publish_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nb = pv[P_NB], nlb = pv[P_NLB], nl = pv[P_NL], rank = pv[P_RANK];
  ocrGuid_t table = depv[1].guid, map = depv[2].guid;
  ocrGuid_t *tab = depv[1].ptr;
  ocrGuid_t *bmap = (ocrGuid_t *)depv[2].ptr + nb * nlb;

  for (u64 xl = 0; xl < nlb; ++xl) {
    const ocrGuid_t *names = depv[3 + xl].ptr;
    for (u64 y = 0; y < nb; ++y) {
      tab[xl * nb + y] = names[y];
      bmap[xl * nb + y] = names[nb + y];
    }
  }
  ocrDbRelease(table);
  for (u64 c = 0; c < nl; ++c) {
    ocrGuid_t pt = point(pv, rank, KIND_TABLE, c);
    mirror_edge_open(pt);
    ocrEventSatisfy(pt, table);
  }

  ocrHint_t eh;
  mirror_rank_hint(&eh, rank, OCR_HINT_EDT_T);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_ITER] = 0;
  ocrGuid_t sp;
  ocrEdtCreate(&sp, mirror_u64_guid(pv[P_SPAWN_TPL]), P_COUNT, params, (u32)(nl + 1),
               NULL, EDT_PROP_NONE, &eh, NULL);
  for (u64 q = 0; q < nl; ++q) {
    ocrGuid_t pt = point(pv, q, KIND_TABLE, rank);
    mirror_edge_open(pt);
    ocrAddDependence(pt, sp, (u32)q, DB_MODE_RO);
  }
  ocrDbRelease(map);
  ocrAddDependence(map, sp, (u32)nl, DB_MODE_RW);

  for (u64 xl = 0; xl < nlb; ++xl) ocrDbDestroy(depv[3 + xl].guid);
  return NULL_GUID;
}

/* One square, in the origin's two forms: tiled when a tile is smaller than
 * the block, whole otherwise. */
static ocrGuid_t xpose_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 bo = pv[P_BO], ts = pv[P_TILE];
  const double *A = depv[0].ptr;
  double *B = depv[1].ptr;

  if (ts < bo) {
    for (u64 i = 0; i < bo; i += ts) {
      for (u64 j = 0; j < bo; j += ts) {
        u64 max_i = i + ts < bo ? i + ts : bo;
        u64 max_j = j + ts < bo ? j + ts : bo;
        for (u64 it = i; it != max_i; ++it)
          for (u64 jt = j; jt != max_j; ++jt)
            B[it + bo * jt] = A[jt + bo * it];
      }
    }
  } else {
    for (u64 i = 0; i != bo; ++i)
      for (u64 j = 0; j != bo; ++j)
        B[i + bo * j] = A[j + bo * i];
  }
  return NULL_GUID;
}

/* A rank's last turn, reported to the rank that ends the program.  The edge
 * carries no data -- its reader only waits on it -- so it is raised from a
 * body, and it has one producer and one consumer, which keeps its
 * destruction unambiguous. */
static ocrGuid_t done_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  ocrGuid_t d = point(pv, pv[P_RANK], KIND_DONE, 0);
  mirror_edge_open(d);
  ocrEventSatisfy(d, NULL_GUID);
  return NULL_GUID;
}

static ocrGuid_t check_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nb = pv[P_NB], bo = pv[P_BO], b = pv[P_BLOCK];
  double errsq = 0.0;
  for (u64 p = 0; p < nb; ++p) {
    const double *t = depv[1 + p].ptr;
    for (u64 r = 0; r < bo; ++r) {
      double col_val = COL_SHIFT * (double)(p * bo + r);
      for (u64 c = 0; c < bo; ++c) {
        double diff = t[r * bo + c] - (col_val + ROW_SHIFT * (double)(b * bo + c));
        errsq += diff * diff;
      }
    }
  }
  ocrHint_t dh;
  mirror_rank_hint(&dh, 0, OCR_HINT_DB_T);
  ocrGuid_t result;
  double *out;
  ocrDbCreate(&result, (void **)&out, sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
  *out = errsq;
  ocrDbRelease(result);
  return result;
}

static ocrGuid_t reduce_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  measurements_t *total = depv[1].ptr;
  double errsq = 0.0;
  for (u64 b = 0; b < pv[P_NLB]; ++b) {
    errsq += *(const double *)depv[2 + b].ptr;
    ocrDbDestroy(depv[2 + b].guid);
  }
  total->error += errsq;
  if (pv[P_ITER] + 1 == pv[P_ITERS]) {
    if (total->error < EPSILON) {
      PRINTF("Solution validates\n");
      u64 order = pv[P_BO] * pv[P_NB];
      total->average /= (double)(pv[P_ITERS] > 1 ? pv[P_ITERS] - 1 : 1);
      PRINTF("Rate (MB/s): %g, Avg time (s): %g, Min time (s): %g, Max time (s): %g\n",
             1.e-6 * (double)(2 * sizeof(double) * order * order) / total->minimum,
             total->average, total->minimum, total->maximum);
      if (pv[P_VERBOSE]) PRINTF("Squared errors: %g\n", total->error);
    } else {
      PRINTF("ERROR: Aggregate squared error %e exceeds threshold %e\n", total->error, EPSILON);
      ocrShutdown();
      return NULL_GUID;
    }
    mirror_app_e2e(pv[P_START_NS]);
    PRINTF("ERRSQ %.6e\n", total->error);
  }
  return NULL_GUID;
}

static ocrGuid_t elapsed_edt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)dc;
  double elapsed = (double)(mirror_now_ns() - pv[P_ITER_START]) * 1.e-9;
  measurements_t *m = dv[1].ptr;
  if (pv[P_ITER] > 0 || pv[P_ITERS] == 1) {
    m->average += elapsed;
    if (elapsed < m->minimum) m->minimum = elapsed;
    if (elapsed > m->maximum) m->maximum = elapsed;
  }
  return NULL_GUID;
}

static ocrGuid_t shutdown_edt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)dc; (void)dv;
  for (u64 q = 1; q < pv[P_NL]; ++q)
    ocrEventDestroy(point(pv, q, KIND_DONE, 0));
  ocrShutdown();
  return NULL_GUID;
}

static ocrGuid_t blockjoin_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc; (void)depv;
  return NULL_GUID;
}

static ocrGuid_t blockspawn_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nb = pv[P_NB], nlb = pv[P_NLB], bl = pv[P_BLOCK];
  const ocrGuid_t *amap = depv[0].ptr, *bmap = amap + nb * nlb;
  ocrHint_t h;
  mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
  ocrGuid_t join = mirror_latch(nb), tail, tail_out;
  ocrEdtCreate(&tail, mirror_u64_guid(pv[P_BLOCKJOIN_TPL]), P_COUNT, pv, 1,
               NULL, EDT_PROP_NONE, &h, &tail_out);
  ocrAddDependence(tail_out, mirror_u64_guid(pv[P_JOIN]),
                   OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  ocrAddDependence(join, tail, 0, DB_MODE_NULL);
  for (u64 p = 0; p < nb; ++p) {
    ocrGuid_t e, out;
    ocrEdtCreate(&e, mirror_u64_guid(pv[P_XPOSE_TPL]), P_COUNT, pv, 2, NULL,
                 EDT_PROP_NONE, &h, &out);
    ocrAddDependence(out, join, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(amap[p * nlb + bl], e, 0, DB_MODE_RO);
    ocrAddDependence(bmap[bl * nb + p], e, 1, DB_MODE_RW);
  }
  return NULL_GUID;
}

/* Register every join consumer before launching its producers. The root's
 * next turn follows validation so readers never overlap the next writers. */
static ocrGuid_t spawn_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nb = pv[P_NB], nlb = pv[P_NLB], nl = pv[P_NL], rank = pv[P_RANK];
  u64 iter = pv[P_ITER];
  int last = iter + 1 == pv[P_ITERS];
  ocrGuid_t map_guid = mirror_u64_guid(pv[P_MAP]);
  ocrGuid_t *map = depv[depc - 1].ptr;
  ocrGuid_t *amap = map, *bmap = map + nb * nlb;

  /* The first turn also takes delivery of the exchange: every rank's
   * sub-block names, re-indexed by the (phase, own block) pair that reads
   * them. */
  if (iter == 0) {
    for (u64 q = 0; q < nl; ++q) {
      const ocrGuid_t *tab = depv[q].ptr;
      for (u64 xl = 0; xl < nlb; ++xl)
        for (u64 bl = 0; bl < nlb; ++bl)
          amap[(q * nlb + xl) * nlb + bl] = tab[xl * nb + rank * nlb + bl];
    }
  }

  ocrGuid_t join = mirror_latch(nlb);
  ocrHint_t h;
  mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  ocrGuid_t next = NULL_GUID;
  params[P_ITER_START] = mirror_now_ns();
  ocrGuid_t elapsed, timed;
  ocrEdtCreate(&elapsed, mirror_u64_guid(pv[P_ELAPSED_TPL]), P_COUNT, params,
               2, NULL, EDT_PROP_NONE, &h, &timed);
  ocrAddDependence(join, elapsed, 0, DB_MODE_NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_ACC]), elapsed, 1, DB_MODE_RW);

  if (rank == 0) {
    ocrGuid_t chk, chk_out = NULL_GUID;
    ocrEdtCreate(&chk, mirror_u64_guid(pv[P_REDUCE_TPL]), P_COUNT, params,
                 (u32)(2 + nlb), NULL, EDT_PROP_NONE, &h, &chk_out);
    if (!last) {
      params[P_ITER] = iter + 1;
      ocrEdtCreate(&next, mirror_u64_guid(pv[P_SPAWN_TPL]), P_COUNT, params, 2,
                   NULL, EDT_PROP_NONE, &h, NULL);
      ocrAddDependence(chk_out, next, 0, DB_MODE_NULL);
    }
    ocrAddDependence(timed, chk, 0, DB_MODE_NULL);
    ocrAddDependence(mirror_u64_guid(pv[P_ACC]), chk, 1, DB_MODE_RW);
    params[P_ITER] = iter;
    for (u64 bl = 0; bl < nlb; ++bl) {
      params[P_BLOCK] = bl;
      ocrGuid_t check, out;
      ocrEdtCreate(&check, mirror_u64_guid(pv[P_CHECK_TPL]), P_COUNT, params,
                   (u32)(1 + nb), NULL, EDT_PROP_NONE, &h, &out);
      ocrAddDependence(out, chk, (u32)(2 + bl), DB_MODE_RO);
      ocrAddDependence(timed, check, 0, DB_MODE_NULL);
      for (u64 p = 0; p < nb; ++p)
        ocrAddDependence(bmap[bl * nb + p], check, (u32)(1 + p), DB_MODE_RO);
    }
    /* Shutdown requires every rank's final completion, not only local work. */
    if (last) {
      ocrGuid_t shutdown;
      ocrEdtCreate(&shutdown, mirror_u64_guid(pv[P_SHUTDOWN_TPL]), P_COUNT, params,
                   (u32)nl, NULL, EDT_PROP_NONE, &h, NULL);
      ocrAddDependence(chk_out, shutdown, 0, DB_MODE_NULL);
      for (u64 q = 1; q < nl; ++q) {
        ocrGuid_t d = point(pv, q, KIND_DONE, 0);
        mirror_edge_open(d);
        ocrAddDependence(d, shutdown, (u32)q, DB_MODE_NULL);
      }
    }
  } else if (!last) {
    params[P_ITER] = iter + 1;
    ocrEdtCreate(&next, mirror_u64_guid(pv[P_SPAWN_TPL]), P_COUNT, params, 2,
                 NULL, EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(timed, next, 0, DB_MODE_NULL);
  } else {
    ocrGuid_t rep;
    ocrEdtCreate(&rep, mirror_u64_guid(pv[P_DONE_TPL]), P_COUNT, params, 1, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(timed, rep, 0, DB_MODE_NULL);
  }

  if (iter == 0)
    for (u64 q = 0; q < nl; ++q) ocrEventDestroy(point(pv, q, KIND_TABLE, rank));

  /* Publish the map before any child or successor acquires it. */
  ocrDbRelease(map_guid);
  if (!ocrGuidIsNull(next)) ocrAddDependence(map_guid, next, 1, DB_MODE_RO);
  params[P_ITER] = iter;
  params[P_JOIN] = mirror_guid_u64(join);
  for (u64 bl = 0; bl < nlb; ++bl) {
    params[P_BLOCK] = bl;
    ocrGuid_t block;
    ocrEdtCreate(&block, mirror_u64_guid(pv[P_BLOCKSPAWN_TPL]), P_COUNT, params,
                 1, NULL, EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(map_guid, block, 0, DB_MODE_RO);
  }
  return NULL_GUID;
}

/* One rank's setup: the name blocks its fill tasks write into, one fill task
 * per own column block, and the task that publishes the names once they are
 * all there. */
static ocrGuid_t driver_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 nb = pv[P_NB], nlb = pv[P_NLB], rank = pv[P_RANK];
  pv[P_START_NS] = mirror_now_ns();

  ocrHint_t dh, eh;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  mirror_rank_hint(&eh, rank, OCR_HINT_EDT_T);

  ocrGuid_t map, table;
  ocrGuid_t *m, *tab;
  ocrDbCreate(&map, (void **)&m, 2 * nb * nlb * sizeof(ocrGuid_t), DB_PROP_NONE, &dh, NO_ALLOC);
  ocrDbRelease(map);
  ocrDbCreate(&table, (void **)&tab, nlb * nb * sizeof(ocrGuid_t), DB_PROP_NONE, &dh, NO_ALLOC);
  ocrDbRelease(table);

  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_MAP] = mirror_guid_u64(map);
  {
    ocrGuid_t acc;
    measurements_t *total;
    ocrDbCreate(&acc, (void **)&total, sizeof(*total), DB_PROP_NONE, &dh, NO_ALLOC);
    *total = (measurements_t){.minimum = DBL_MAX};
    ocrDbRelease(acc);
    params[P_ACC] = mirror_guid_u64(acc);
  }

  /* A fill task writes only its own slice of the names, so the concurrent
   * fillers share no block; the slice is the one block each of them writes
   * and the publisher is its one reader. */
  ocrGuid_t filled = mirror_latch(nlb);
  ocrGuid_t pub;
  ocrEdtCreate(&pub, mirror_u64_guid(pv[P_PUBLISH_TPL]), P_COUNT, params,
               (u32)(3 + nlb), NULL, EDT_PROP_NONE, &eh, NULL);
  ocrAddDependence(filled, pub, 0, DB_MODE_NULL);
  ocrAddDependence(table, pub, 1, DB_MODE_RW);
  ocrAddDependence(map, pub, 2, DB_MODE_RW);

  for (u64 bl = 0; bl < nlb; ++bl) {
    ocrGuid_t slice;
    ocrGuid_t *s;
    ocrDbCreate(&slice, (void **)&s, 2 * nb * sizeof(ocrGuid_t), DB_PROP_NONE, &dh, NO_ALLOC);
    ocrDbRelease(slice);
    ocrAddDependence(slice, pub, (u32)(3 + bl), DB_MODE_RO);
    params[P_BLOCK] = bl;
    ocrGuid_t f, out;
    ocrEdtCreate(&f, mirror_u64_guid(pv[P_FILL_TPL]), P_COUNT, params, 1, NULL,
                 EDT_PROP_NONE, &eh, &out);
    ocrAddDependence(out, filled, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(slice, f, 0, DB_MODE_RW);
  }
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 order = 1024, iters = 10, nlb = 1, tile = 0;
  int tiled = mirror_option_u64(argdb, argc, "--tile_size", &tile);
  int bad = mirror_option_u64(argdb, argc, "--matrix_size", &order) < 0
         || mirror_option_u64(argdb, argc, "--iterations", &iters) < 0
         || mirror_option_u64(argdb, argc, "--num_blocks", &nlb) < 0
         || tiled < 0;
  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  /* The origin's untiled default is a tile as wide as the matrix. */
  if (tiled == 0) tile = order;
  /* The block count is a product of an argument and the rank count, so the
   * factor is bounded before the product is formed: a wrapped product would
   * divide by zero in the very test that exists to reject absurd input.
   * Bounding it by `order / nl` also makes a square at least one element
   * wide, which every runnable geometry needs anyway. */
  int sane = !bad && nl != 0 && order != 0 && iters != 0 && tile != 0
             && nlb != 0 && nlb <= order / nl;
  u64 nb = sane ? nl * nlb : 1;
  if (!sane || order % nb) {
    PRINTF("transpose_hpx: usage --matrix_size=N --iterations=I --num_blocks=B"
           " [--tile_size=T] (B blocks per rank, B >= 1, I >= 1, T >= 1,"
           " N a multiple of B times the rank count)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t fill_tpl, publish_tpl, xpose_tpl, spawn_tpl, check_tpl, done_tpl, driver_tpl;
  ocrGuid_t blockspawn_tpl, blockjoin_tpl, reduce_tpl;
  ocrGuid_t elapsed_tpl, shutdown_tpl;
  ocrEdtTemplateCreate(&fill_tpl, fill_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&publish_tpl, publish_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&xpose_tpl, xpose_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&spawn_tpl, spawn_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&check_tpl, check_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&reduce_tpl, reduce_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&blockspawn_tpl, blockspawn_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&blockjoin_tpl, blockjoin_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&done_tpl, done_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&driver_tpl, driver_edt, P_COUNT, 0);
  ocrEdtTemplateCreate(&elapsed_tpl, elapsed_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&shutdown_tpl, shutdown_edt, P_COUNT, EDT_PARAM_UNK);

  /* One name per kind per rank per consumer: the exchange and the completion
   * report are the whole point space. */
  ocrGuid_t range;
  ocrGuidRangeCreate(&range, KIND_COUNT * nl * nl, GUID_USER_EVENT_STICKY);

  u64 pv[P_COUNT] = {0};
  pv[P_ITERS] = iters; pv[P_TILE] = tile;
  pv[P_NLB] = nlb; pv[P_NB] = nb; pv[P_BO] = order / nb; pv[P_NL] = nl;
  pv[P_RANGE] = mirror_guid_u64(range);
  pv[P_FILL_TPL] = mirror_guid_u64(fill_tpl);
  pv[P_PUBLISH_TPL] = mirror_guid_u64(publish_tpl);
  pv[P_XPOSE_TPL] = mirror_guid_u64(xpose_tpl);
  pv[P_SPAWN_TPL] = mirror_guid_u64(spawn_tpl);
  pv[P_CHECK_TPL] = mirror_guid_u64(check_tpl);
  pv[P_REDUCE_TPL] = mirror_guid_u64(reduce_tpl);
  pv[P_BLOCKSPAWN_TPL] = mirror_guid_u64(blockspawn_tpl);
  pv[P_BLOCKJOIN_TPL] = mirror_guid_u64(blockjoin_tpl);
  pv[P_DONE_TPL] = mirror_guid_u64(done_tpl);
  pv[P_ELAPSED_TPL] = mirror_guid_u64(elapsed_tpl);
  pv[P_SHUTDOWN_TPL] = mirror_guid_u64(shutdown_tpl);
  for (u64 i = 1; i < argc; ++i)
    if (strcmp(getArgv(argdb, i), "--verbose") == 0) pv[P_VERBOSE] = 1;

  mirror_spmd_fork(driver_tpl, pv, P_COUNT, P_RANK, nl, 0, NULL);
  return NULL_GUID;
}
