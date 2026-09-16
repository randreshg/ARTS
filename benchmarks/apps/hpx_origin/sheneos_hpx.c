/* OCR mirror of HPX's sheneos example: a tabulated nuclear equation of state,
 * cut into a cube of three-dimensional partitions, and a set of workers that
 * each interpolate the same grid of query points out of it.  A partition
 * reads its own slab of the table where it lives; a worker groups its points
 * by the partition that owns them and sends one bulk request per partition,
 * which is the origin's one action per partition and its one reply. */
#include "hpx_mirror.h"
#include "shen_rng.h"

#include <hdf5.h>
#include <math.h>
#include <time.h>

enum {
  P_NYE, P_NTEMP, P_NRHO, P_NPART, P_NWORKERS, P_SEED,
  P_PPD, P_LIVE, P_NL, P_NTOTAL,
  P_RANK, P_WORKER, P_PART, P_NCOORD,
  P_OFF_YE, P_OFF_TEMP, P_OFF_RHO, P_CNT_YE, P_CNT_TEMP, P_CNT_RHO,
  P_RANGE, P_SUM, P_START_NS,
  P_WORKER_TPL, P_QUERY_TPL, P_COLLECT_TPL, P_READ_TPL, P_REPLY_TPL,
  P_COUNT
};

#define SHEN_DIM 3
#define SHEN_VALUES 8
#define SHEN_PATH_MAX 480

/* The three independent axes and the eight dependent quantities, in the
 * order the origin reads and returns them -- the order is the reply's
 * layout, so it is part of the interface and not a detail. */
static const char *const SHEN_AXIS[SHEN_DIM] = {"ye", "logtemp", "logrho"};
static const char *const SHEN_FIELD[SHEN_VALUES] = {
    "logpress", "logenergy", "entropy", "munu",
    "cs2", "dedt", "dpdrhoe", "dpderho"};

/* What every worker needs and no worker can derive: the axis metadata the
 * client reads from the table, the table's own path, and the partitions'
 * names.  One block, read-only, replicated by acquire rather than by copy. */
typedef struct {
  u64 num_values[SHEN_DIM];
  double minval[SHEN_DIM];
  double maxval[SHEN_DIM];
  double delta[SHEN_DIM];
  char file[SHEN_PATH_MAX];
  ocrGuid_t part[];
} shen_table_t;

/* One partition: the slab's shape, the range it answers for, and its data --
 * the three axis slices followed by the eight value arrays. */
typedef struct {
  u64 count[SHEN_DIM];
  double min_value[SHEN_DIM];
  double max_value[SHEN_DIM];
  double delta[SHEN_DIM];
  double energy_shift;
  double data[];
} shen_part_t;

static inline u64 shen_cells(const u64 *count) {
  return count[0] * count[1] * count[2];
}
static inline u64 shen_part_bytes(const u64 *count) {
  return (u64)sizeof(shen_part_t) +
         (count[0] + count[1] + count[2] + SHEN_VALUES * shen_cells(count)) *
             sizeof(double);
}
static inline double *shen_axis(shen_part_t *p, int d) {
  double *a = p->data;
  for (int i = 0; i < d; ++i) a += p->count[i];
  return a;
}
static inline double *shen_field(shen_part_t *p, int v) {
  return p->data + p->count[0] + p->count[1] + p->count[2] +
         (u64)v * shen_cells(p->count);
}

/* A slab that is not the last one along its axis holds two samples past its
 * own right edge, so a point in its last cell still has the neighbour the
 * interpolation reads; the range it answers for covers one of them. */
static inline int shen_has_ghost(u64 offset, u64 count, u64 size) {
  return size > 2 && offset + count < size - 2;
}
static inline u64 shen_held(u64 offset, u64 count, u64 size) {
  return shen_has_ghost(offset, count, size) ? count + 2 : count;
}
static inline u64 shen_spanned(u64 offset, u64 count, u64 size) {
  return shen_has_ghost(offset, count, size) ? count + 1 : count;
}

/* ---- the table file -------------------------------------------------- */

/* One hyperslab of one dataset.  The file is opened per read, as the origin
 * opens it per read. */
static int shen_read(const char *path, const char *name, int rank,
                     const hsize_t *off, const hsize_t *cnt, double *out) {
  hid_t f = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (f < 0) return -1;
  int rc = -1;
  hid_t d = H5Dopen2(f, name, H5P_DEFAULT);
  if (d >= 0) {
    hid_t fs = H5Dget_space(d);
    hid_t ms = H5Screate_simple(rank, cnt, NULL);
    if (fs >= 0 && ms >= 0 &&
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, off, NULL, cnt, NULL) >= 0 &&
        H5Dread(d, H5T_NATIVE_DOUBLE, ms, fs, H5P_DEFAULT, out) >= 0)
      rc = 0;
    if (ms >= 0) H5Sclose(ms);
    if (fs >= 0) H5Sclose(fs);
    H5Dclose(d);
  }
  H5Fclose(f);
  return rc;
}

/* An axis's first and last samples, its step and its length.  The step is the
 * difference of the first two samples: the table's axes are uniform, and the
 * whole index arithmetic on both sides rests on that. */
static int shen_range(const char *path, const char *name, double *min,
                      double *max, double *delta, u64 *n) {
  hid_t f = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (f < 0) return -1;
  int rc = -1;
  hid_t d = H5Dopen2(f, name, H5P_DEFAULT);
  if (d >= 0) {
    hid_t fs = H5Dget_space(d);
    hsize_t dims[1] = {0};
    if (fs >= 0 && H5Sget_simple_extent_ndims(fs) == 1 &&
        H5Sget_simple_extent_dims(fs, dims, NULL) == 1 && dims[0] >= 2) {
      hsize_t one = 1;
      hid_t ms = H5Screate_simple(1, &one, NULL);
      double second = 0.0;
      hsize_t at[3];
      double *into[3];
      at[0] = 0; into[0] = min;
      at[1] = dims[0] - 1; into[1] = max;
      at[2] = 1; into[2] = &second;
      int ok = ms >= 0;
      for (int i = 0; ok && i < 3; ++i)
        ok = H5Sselect_hyperslab(fs, H5S_SELECT_SET, &at[i], NULL, &one, NULL) >= 0 &&
             H5Dread(d, H5T_NATIVE_DOUBLE, ms, fs, H5P_DEFAULT, into[i]) >= 0;
      if (ok) {
        *delta = second - *min;
        *n = (u64)dims[0];
        rc = 0;
      }
      if (ms >= 0) H5Sclose(ms);
    }
    if (fs >= 0) H5Sclose(fs);
    H5Dclose(d);
  }
  H5Fclose(f);
  return rc;
}

/* ---- the interpolation kernel ---------------------------------------- */

static inline u64 shen_cell(const shen_part_t *p, u64 x, u64 y, u64 z) {
  return z + (y + x * p->count[1]) * p->count[2];
}

/* Trilinear interpolation over the cell the three indices open, in the
 * origin's own summation order. */
static double shen_tl(const shen_part_t *p, const double *v, u64 ix, u64 iy,
                      u64 iz, double dx, double dy, double dz) {
  double v000 = v[shen_cell(p, ix, iy, iz)];
  double v001 = v[shen_cell(p, ix, iy, iz + 1)];
  double v010 = v[shen_cell(p, ix, iy + 1, iz)];
  double v011 = v[shen_cell(p, ix, iy + 1, iz + 1)];
  double v100 = v[shen_cell(p, ix + 1, iy, iz)];
  double v101 = v[shen_cell(p, ix + 1, iy, iz + 1)];
  double v110 = v[shen_cell(p, ix + 1, iy + 1, iz)];
  double v111 = v[shen_cell(p, ix + 1, iy + 1, iz + 1)];
  double cx = 1. - dx, cy = 1. - dy, cz = 1. - dz;
  return v000 * cx * cy * cz + v001 * cx * cy * dz + v010 * cx * dy * cz +
         v011 * cx * dy * dz + v100 * dx * cy * cz + v101 * dx * cy * dz +
         v110 * dx * dy * cz + v111 * dx * dy * dz;
}

/* The origin throws when a value falls outside the slab it asks; a query that
 * reaches the wrong partition is a routing defect, so the mirror reports it
 * rather than clamping it into an answer. */
static inline int shen_index_of(const shen_part_t *p, int d, double value,
                                u64 *out) {
  if (!(value >= p->min_value[d]) || !(value <= p->max_value[d])) return -1;
  *out = (u64)((value - p->min_value[d]) / p->delta[d]);
  return 0;
}

/* The eight quantities for one point: the two logarithmic ones are returned
 * as powers of ten, and the energy carries the table's own shift. */
static int shen_interpolate(shen_part_t *p, double ye, double temp, double rho,
                            double *out) {
  double logtemp = log10(temp), logrho = log10(rho);
  u64 ix, iy, iz;
  if (shen_index_of(p, 0, ye, &ix) || shen_index_of(p, 1, logtemp, &iy) ||
      shen_index_of(p, 2, logrho, &iz))
    return -1;
  double dx = (ye - shen_axis(p, 0)[ix]) / p->delta[0];
  double dy = (logtemp - shen_axis(p, 1)[iy]) / p->delta[1];
  double dz = (logrho - shen_axis(p, 2)[iz]) / p->delta[2];
  for (int v = 0; v < SHEN_VALUES; ++v) {
    double t = shen_tl(p, shen_field(p, v), ix, iy, iz, dx, dy, dz);
    out[v] = v == 0   ? pow(10., t)
             : v == 1 ? pow(10., t) - p->energy_shift
                      : t;
  }
  return 0;
}

/* Which partition answers for a value: the axis is cut into `ppd` pieces of
 * `num_values/ppd` samples each, and the value at the very top edge belongs
 * to the last piece. */
static inline u64 shen_slab_of(const shen_table_t *t, int d, u64 ppd,
                               double value) {
  u64 psize = t->num_values[d] / ppd;
  double where = (value - t->minval[d]) / (t->delta[d] * (double)psize);
  u64 i = (u64)where;
  if (i == ppd) --i;
  return i;
}
static inline u64 shen_partition_of(const shen_table_t *t, u64 ppd, double ye,
                                    double temp, double rho) {
  u64 x = shen_slab_of(t, 0, ppd, ye);
  u64 y = shen_slab_of(t, 1, ppd, log10(temp));
  u64 z = shen_slab_of(t, 2, ppd, log10(rho));
  return x + (y + z * ppd) * ppd;
}

/* The reply of one (worker, partition) pair.  One producer, one consumer, and
 * an ordinal that names the pair exactly once. */
static inline ocrGuid_t shen_reply_point(u64 *pv, u64 worker_global, u64 part) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]),
                     worker_global * pv[P_LIVE] + part, pv[P_RANK], pv[P_NL]);
}

/* ---- the tasks -------------------------------------------------------- */

/* One partition's slab, read where the partition lives.  This is the whole
 * of the program's file input and the reason the row's opening phase is I/O:
 * three axis slices, the energy shift, and eight three-dimensional
 * hyperslabs. */
static ocrGuid_t init_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  shen_part_t *p = depv[0].ptr;
  const shen_table_t *t = depv[1].ptr;
  u64 offset[SHEN_DIM] = {pv[P_OFF_YE], pv[P_OFF_TEMP], pv[P_OFF_RHO]};
  u64 count[SHEN_DIM] = {pv[P_CNT_YE], pv[P_CNT_TEMP], pv[P_CNT_RHO]};

  for (int d = 0; d < SHEN_DIM; ++d) {
    u64 size = t->num_values[d];
    p->count[d] = shen_held(offset[d], count[d], size);
    hsize_t off = (hsize_t)offset[d], cnt = (hsize_t)p->count[d];
    if (shen_read(t->file, SHEN_AXIS[d], 1, &off, &cnt, shen_axis(p, d)) != 0) {
      PRINTF("sheneos_hpx: cannot read axis '%s' of %s\n", SHEN_AXIS[d], t->file);
      ocrShutdown();
      return NULL_GUID;
    }
    const double *a = shen_axis(p, d);
    p->min_value[d] = a[0];
    p->max_value[d] = a[shen_spanned(offset[d], count[d], size) - 1];
    p->delta[d] = a[1] - a[0];
  }

  hsize_t zero = 0, one = 1;
  if (shen_read(t->file, "energy_shift", 1, &zero, &one, &p->energy_shift) != 0) {
    PRINTF("sheneos_hpx: cannot read energy_shift of %s\n", t->file);
    ocrShutdown();
    return NULL_GUID;
  }

  hsize_t off3[SHEN_DIM] = {(hsize_t)offset[0], (hsize_t)offset[1],
                            (hsize_t)offset[2]};
  hsize_t cnt3[SHEN_DIM] = {(hsize_t)p->count[0], (hsize_t)p->count[1],
                            (hsize_t)p->count[2]};
  for (int v = 0; v < SHEN_VALUES; ++v)
    if (shen_read(t->file, SHEN_FIELD[v], SHEN_DIM, off3, cnt3,
                  shen_field(p, v)) != 0) {
      PRINTF("sheneos_hpx: cannot read '%s' of %s\n", SHEN_FIELD[v], t->file);
      ocrShutdown();
      return NULL_GUID;
    }
  return NULL_GUID;
}

/* One (worker, partition) request: interpolate every point of the request and
 * publish the reply on the pair's own point. */
static ocrGuid_t query_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  shen_part_t *p = depv[0].ptr;
  const double *coord = depv[1].ptr;
  u64 n = pv[P_NCOORD];

  ocrHint_t dh;
  mirror_here_hint(&dh, OCR_HINT_DB_T);
  ocrGuid_t reply;
  double *out;
  ocrDbCreate(&reply, (void **)&out, n * SHEN_VALUES * sizeof(double),
              DB_PROP_NONE, &dh, NO_ALLOC);
  for (u64 i = 0; i < n; ++i)
    if (shen_interpolate(p, coord[3 * i], coord[3 * i + 1], coord[3 * i + 2],
                         out + i * SHEN_VALUES) != 0) {
      PRINTF("sheneos_hpx: point %lu reached partition %lu, which does not "
             "hold it\n", (unsigned long)i, (unsigned long)pv[P_PART]);
      ocrShutdown();
      return NULL_GUID;
    }
  ocrDbRelease(reply);

  ocrGuid_t pt = shen_reply_point(pv, pv[P_RANK] * pv[P_NWORKERS] + pv[P_WORKER],
                                  pv[P_PART]);
  mirror_edge_open(pt);
  ocrEventSatisfy(pt, reply);
  ocrDbDestroy(depv[1].guid);
  return NULL_GUID;
}

static ocrGuid_t reply_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrHint_t dh;
  mirror_rank_hint(&dh, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t copy;
  double *out;
  u64 bytes = pv[P_NCOORD] * SHEN_VALUES * sizeof(double);
  ocrDbCreate(&copy, (void **)&out, bytes, DB_PROP_NONE, &dh, NO_ALLOC);
  memcpy(out, depv[0].ptr, bytes);
  ocrDbRelease(copy);
  ocrDbDestroy(depv[0].guid);
  u64 worker = pv[P_RANK] * pv[P_NWORKERS] + pv[P_WORKER];
  ocrEventDestroy(shen_reply_point(pv, worker, pv[P_PART]));
  return copy;
}

static ocrGuid_t collect_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  const u64 *plan = depv[0].ptr;
  u64 worker_global = pv[P_RANK] * pv[P_NWORKERS] + pv[P_WORKER];
  const u64 *order = plan + 2 * (depc - 1);
  u64 points = pv[P_NYE] * pv[P_NTEMP] * pv[P_NRHO];
  double total = 0.0;
  for (u64 i = 0; i < points; ++i) {
    const double *r = depv[1 + order[2 * i]].ptr;
    r += order[2 * i + 1] * SHEN_VALUES;
    for (u64 v = 0; v < SHEN_VALUES; ++v) total += r[v];
  }

  ocrHint_t dh;
  mirror_rank_hint(&dh, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t share;
  double *out;
  ocrDbCreate(&share, (void **)&out, sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
  *out = total;
  ocrDbRelease(share);

  for (u32 s = 1; s < depc; ++s) {
    ocrDbDestroy(depv[s].guid);
  }
  ocrDbDestroy(depv[0].guid);
  /* The share goes in last: it is what the end task waits for, so anything
   * this task still has to do belongs before the push and not beside the
   * shutdown it releases. */
  ocrAddDependence(share, mirror_u64_guid(pv[P_SUM]), (u32)worker_global,
                   DB_MODE_RO);
  return NULL_GUID;
}

static ocrGuid_t read_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  const shen_table_t *t = depv[0].ptr;
  ocrHint_t dh;
  mirror_rank_hint(&dh, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t ranges;
  double *values;
  ocrDbCreate(&ranges, (void **)&values, 2 * SHEN_DIM * sizeof(double),
              DB_PROP_NONE, &dh, NO_ALLOC);
  for (int d = 0; d < SHEN_DIM; ++d) {
    double min, max, delta;
    u64 n;
    if (shen_range(t->file, SHEN_AXIS[d], &min, &max, &delta, &n) != 0) {
      PRINTF("sheneos_hpx: cannot read axis '%s' of %s\n", SHEN_AXIS[d], t->file);
      ocrShutdown();
      return NULL_GUID;
    }
    values[d] = d == 0 ? min : pow(10., min);
    values[SHEN_DIM + d] = d == 0 ? max : pow(10., max);
  }
  ocrDbRelease(ranges);
  return ranges;
}

static void shen_shuffle(shen_rng_t *stream, u64 *sequence, u64 n) {
  for (u64 i = 1; i < n; ++i) {
    u64 j = (u64)shen_rng_draw(stream) % (i + 1);
    u64 tmp = sequence[i];
    sequence[i] = sequence[j];
    sequence[j] = tmp;
  }
}

static ocrGuid_t worker_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  shen_table_t *t = depv[0].ptr;
  u64 ppd = pv[P_PPD], live = pv[P_LIVE], nl = pv[P_NL];
  u64 npts[SHEN_DIM] = {pv[P_NYE], pv[P_NTEMP], pv[P_NRHO]};
  const double *range = depv[1].ptr;
  double lo[SHEN_DIM], step[SHEN_DIM];
  for (int d = 0; d < SHEN_DIM; ++d) {
    lo[d] = range[d];
    step[d] = (range[SHEN_DIM + d] - lo[d]) / (double)npts[d];
  }
  ocrDbDestroy(depv[1].guid);

  /* The samples are accumulated, not multiplied out: the origin walks each
   * axis by repeated addition and the two must round the same way. */
  double *axis[SHEN_DIM];
  u64 *sequence[SHEN_DIM];
  for (int d = 0; d < SHEN_DIM; ++d) {
    axis[d] = malloc(npts[d] * sizeof(double));
    sequence[d] = malloc(npts[d] * sizeof(u64));
    double v = lo[d];
    for (u64 i = 0; i < npts[d]; ++i) {
      axis[d][i] = v;
      sequence[d][i] = i;
      v += step[d];
    }
  }

  shen_rng_t *stream = depv[2].ptr;
  shen_rng_seed(stream, (unsigned int)(pv[P_SEED] + pv[P_RANK]));
  for (int d = 0; d < SHEN_DIM; ++d)
    shen_shuffle(stream, sequence[d], npts[d]);
  ocrDbRelease(depv[2].guid);

  u64 *count = calloc(live, sizeof(u64));
  for (u64 i = 0; i < npts[0]; ++i)
    for (u64 j = 0; j < npts[1]; ++j)
      for (u64 k = 0; k < npts[2]; ++k)
        ++count[shen_partition_of(t, ppd, axis[0][sequence[0][i]], axis[1][sequence[1][j]], axis[2][sequence[2][k]])];

  u32 groups = 0;
  for (u64 p = 0; p < live; ++p)
    if (count[p]) ++groups;

  ocrHint_t eh;
  mirror_rank_hint(&eh, pv[P_RANK], OCR_HINT_EDT_T);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  ocrGuid_t collect;
  ocrEdtCreate(&collect, mirror_u64_guid(pv[P_COLLECT_TPL]), P_COUNT, params,
               groups + 1, NULL, EDT_PROP_NONE, &eh, NULL);

  ocrHint_t dh;
  mirror_rank_hint(&dh, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t plan;
  u64 *planp;
  u64 points = npts[0] * npts[1] * npts[2];
  ocrDbCreate(&plan, (void **)&planp, 2 * (groups + points) * sizeof(u64),
              DB_PROP_NONE, &dh, NO_ALLOC);
  u64 *order = planp + 2 * groups;

  /* One request block per group, filled in the order the points are
   * enumerated, so a reply's values arrive in their group's own order. */
  ocrGuid_t *req = malloc((size_t)groups * sizeof(ocrGuid_t));
  double **into = malloc((size_t)groups * sizeof(double *));
  u32 *slot = malloc((size_t)live * sizeof(u32));
  u32 g = 0;
  for (u64 p = 0; p < live; ++p) {
    if (!count[p]) continue;
    slot[p] = g;
    planp[2 * g] = p;
    planp[2 * g + 1] = count[p];
    ocrDbCreate(&req[g], (void **)&into[g], 3 * count[p] * sizeof(double),
                DB_PROP_NONE, &dh, NO_ALLOC);
    ++g;
  }
  u64 *fill = calloc(groups, sizeof(u64));
  for (u64 i = 0; i < npts[0]; ++i)
    for (u64 j = 0; j < npts[1]; ++j)
      for (u64 k = 0; k < npts[2]; ++k) {
        double ye = axis[0][sequence[0][i]];
        double temp = axis[1][sequence[1][j]];
        double rho = axis[2][sequence[2][k]];
        u32 s = slot[shen_partition_of(t, ppd, ye, temp, rho)];
        u64 ordinal = (i * npts[1] + j) * npts[2] + k;
        order[2 * ordinal] = s;
        order[2 * ordinal + 1] = fill[s];
        double *w = into[s] + 3 * fill[s]++;
        w[0] = ye;
        w[1] = temp;
        w[2] = rho;
      }

  for (u32 s = 0; s < groups; ++s) {
    u64 p = planp[2 * s];
    ocrDbRelease(req[s]);
    params[P_PART] = p;
    params[P_NCOORD] = planp[2 * s + 1];
    ocrHint_t qh;
    mirror_rank_hint(&qh, mirror_owner(p, pv[P_NPART], nl), OCR_HINT_EDT_T);
    ocrGuid_t q;
    ocrEdtCreate(&q, mirror_u64_guid(pv[P_QUERY_TPL]), P_COUNT, params, 2, NULL,
                 EDT_PROP_NONE, &qh, NULL);
    ocrGuid_t pt =
        shen_reply_point(pv, pv[P_RANK] * pv[P_NWORKERS] + pv[P_WORKER], p);
    mirror_edge_open(pt);
    ocrGuid_t receive, received;
    ocrEdtCreate(&receive, mirror_u64_guid(pv[P_REPLY_TPL]), P_COUNT, params,
                 1, NULL, EDT_PROP_NONE, &eh, &received);
    ocrAddDependence(received, collect, s + 1, DB_MODE_RO);
    ocrAddDependence(pt, receive, 0, DB_MODE_RO);
    ocrAddDependence(t->part[p], q, 0, DB_MODE_RO);
    ocrAddDependence(req[s], q, 1, DB_MODE_RO);
  }

  /* The plan is released only here: the loop above reads the group's
   * partition and its count out of this task's own mapping of it, and a
   * release ends that mapping. */
  ocrDbRelease(plan);
  ocrAddDependence(plan, collect, 0, DB_MODE_RO);

  for (int d = 0; d < SHEN_DIM; ++d) {
    free(axis[d]);
    free(sequence[d]);
  }
  free(count);
  free(req);
  free(into);
  free(slot);
  free(fill);
  return NULL_GUID;
}

/* The workers, once every partition holds its data: the origin's driver
 * spawns `num-workers` of them on every locality from the one it runs on. */
static ocrGuid_t start_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrGuid_t table = depv[1].guid;
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  for (u64 r = 0; r < pv[P_NL]; ++r) {
    params[P_RANK] = r;
    ocrHint_t h;
    mirror_rank_hint(&h, r, OCR_HINT_EDT_T);
    ocrHint_t dh;
    mirror_rank_hint(&dh, r, OCR_HINT_DB_T);
    ocrGuid_t rng;
    shen_rng_t *stream;
    ocrDbCreate(&rng, (void **)&stream, sizeof *stream, DB_PROP_NONE, &dh, NO_ALLOC);
    shen_rng_init(stream);
    ocrDbRelease(rng);
    ocrAddDependence(rng, mirror_u64_guid(pv[P_SUM]),
                     (u32)(pv[P_NTOTAL] + r), DB_MODE_NULL);
    u64 nw = pv[P_NWORKERS];
    ocrGuid_t *reads = malloc(nw * sizeof(ocrGuid_t));
    ocrGuid_t *ready = malloc(nw * sizeof(ocrGuid_t));
    for (u64 w = 0; w < nw; ++w) {
      params[P_WORKER] = w;
      ocrEdtCreate(&reads[w], mirror_u64_guid(pv[P_READ_TPL]), P_COUNT,
                   params, 2, NULL, EDT_PROP_NONE, &h, &ready[w]);
      ocrGuid_t e;
      ocrEdtCreate(&e, mirror_u64_guid(pv[P_WORKER_TPL]), P_COUNT, params, 3,
                   NULL, EDT_PROP_NONE, &h, NULL);
      ocrAddDependence(table, e, 0, DB_MODE_RO);
      ocrAddDependence(ready[w], e, 1, DB_MODE_RO);
      ocrAddDependence(rng, e, 2, DB_MODE_RW);
    }
    /* Register successors before starting the read-only critical sections. */
    for (u64 w = nw; w-- > 0;) {
      ocrAddDependence(w ? ready[w - 1] : NULL_GUID, reads[w], 1, DB_MODE_NULL);
      ocrAddDependence(table, reads[w], 0, DB_MODE_RO);
    }
    free(reads);
    free(ready);
  }

  return NULL_GUID;
}

/* The end, behind every worker's share: the origin's driver sums what its
 * workers returned once all of them have returned it. */
static ocrGuid_t sum_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  mirror_app_e2e(pv[P_START_NS]);
  double total = 0.0;
  for (u64 i = 0; i < pv[P_NTOTAL]; ++i) total += *(const double *)depv[i].ptr;
  PRINTF("CHECKSUM %.14e\n", total);
  for (u32 i = 0; i < depc; ++i) ocrDbDestroy(depv[i].guid);
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  char file[SHEN_PATH_MAX] = {0};
  u64 n_ye = 40, n_temp = 40, n_rho = 40, npart = 32, nworkers = 1, seed = 0;
  int bad = mirror_option_str(argdb, argc, "--file", file, sizeof file) < 0 ||
            mirror_option_u64(argdb, argc, "--num-ye-points", &n_ye) < 0 ||
            mirror_option_u64(argdb, argc, "--num-temp-points", &n_temp) < 0 ||
            mirror_option_u64(argdb, argc, "--num-rho-points", &n_rho) < 0 ||
            mirror_option_u64(argdb, argc, "--num-partitions", &npart) < 0 ||
            mirror_option_u64(argdb, argc, "--num-workers", &nworkers) < 0 ||
            mirror_option_u64(argdb, argc, "--seed", &seed) < 0;
  if (!seed) seed = (u64)time(NULL);
  /* The driver asks for the ye count three times: the temperature and density
   * counts are parsed and then overwritten by it, so the grid is a cube of
   * that one number.  Carried as the origin has it. */
  n_temp = n_ye;
  n_rho = n_ye;

  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  /* The origin cuts the cube by the cube root of the locality count and
   * routes a query by the cube root of the partition count computed through
   * exp/log, which truncates one below at an exact cube; a query routed to a
   * partition the cut never initialised throws there, so the usage gate is
   * the equality of the two truncations, not of the two counts alone. */
  u64 ppd = nl ? (u64)cbrt((double)nl) : 0;
  u64 routed = npart ? (u64)exp(log((double)npart) / 3.0) : 0;
  u64 live = ppd * ppd * ppd;
  u64 ntotal = 1, coords = 1, points = 1;
  int sane = !bad && nl != 0 && file[0] != '\0' && n_ye != 0 && nworkers != 0 &&
             ppd != 0 && npart == nl && routed == ppd;
  if (sane) {
    sane = mirror_fits(&ntotal, nworkers, (u64)1 << 20) &&
           mirror_fits(&ntotal, nl, (u64)1 << 20) &&
           mirror_fits(&coords, n_ye, (u64)1 << 24) &&
           mirror_fits(&coords, n_temp, (u64)1 << 24) &&
           mirror_fits(&coords, n_rho, (u64)1 << 24) &&
           mirror_fits(&points, ntotal, (u64)1 << 26) &&
           mirror_fits(&points, live, (u64)1 << 26) &&
           mirror_fits(&points, nl, (u64)1 << 26);
  }

  u64 start_ns = mirror_now_ns();

  /* The axis metadata the client reads before it cuts the cube: the same
   * three reads the origin performs on the locality that builds it. */
  double minval[SHEN_DIM], maxval[SHEN_DIM], delta[SHEN_DIM];
  u64 nvals[SHEN_DIM];
  if (sane)
    for (int d = 0; d < SHEN_DIM; ++d)
      if (shen_range(file, SHEN_AXIS[d], &minval[d], &maxval[d], &delta[d],
                     &nvals[d]) != 0 ||
          nvals[d] < 4 || nvals[d] / ppd < 2) {
        PRINTF("sheneos_hpx: %s does not hold a usable '%s' axis\n", file,
               SHEN_AXIS[d]);
        ocrShutdown();
        return NULL_GUID;
      }

  ocrGuid_t range = NULL_GUID;
  if (sane && ocrGuidRangeCreate(&range, points, GUID_USER_EVENT_STICKY) != 0)
    sane = 0;
  if (!sane) {
    PRINTF("sheneos_hpx: usage --file=PATH --num-ye-points=N "
           "--num-temp-points=N --num-rho-points=N --num-partitions=P "
           "--num-workers=W --seed=S (P must equal the node count: the origin "
           "cuts the cube by the locality count and routes queries by the "
           "partition count, and the two agree only then; N >= 1, W >= 1, "
           "N^3 < 2^24)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t init_tpl, start_tpl, worker_tpl, query_tpl, collect_tpl, sum_tpl;
  ocrGuid_t read_tpl, reply_tpl;
  ocrEdtTemplateCreate(&init_tpl, init_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&start_tpl, start_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&worker_tpl, worker_edt, P_COUNT, 3);
  ocrEdtTemplateCreate(&query_tpl, query_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&read_tpl, read_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&reply_tpl, reply_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&collect_tpl, collect_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&sum_tpl, sum_edt, P_COUNT, (u32)(ntotal + nl));

  u64 pv[P_COUNT] = {0};
  pv[P_START_NS] = start_ns;
  pv[P_NYE] = n_ye; pv[P_NTEMP] = n_temp; pv[P_NRHO] = n_rho;
  pv[P_NPART] = npart; pv[P_NWORKERS] = nworkers; pv[P_SEED] = seed;
  pv[P_PPD] = ppd; pv[P_LIVE] = live; pv[P_NL] = nl; pv[P_NTOTAL] = ntotal;
  pv[P_RANGE] = mirror_guid_u64(range);
  pv[P_WORKER_TPL] = mirror_guid_u64(worker_tpl);
  pv[P_QUERY_TPL] = mirror_guid_u64(query_tpl);
  pv[P_COLLECT_TPL] = mirror_guid_u64(collect_tpl);
  pv[P_READ_TPL] = mirror_guid_u64(read_tpl);
  pv[P_REPLY_TPL] = mirror_guid_u64(reply_tpl);

  ocrHint_t h0;
  mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrHint_t th;
  mirror_rank_hint(&th, 0, OCR_HINT_DB_T);
  ocrGuid_t table;
  shen_table_t *t;
  ocrDbCreate(&table, (void **)&t,
              sizeof(shen_table_t) + live * sizeof(ocrGuid_t), DB_PROP_NONE,
              &th, NO_ALLOC);
  for (int d = 0; d < SHEN_DIM; ++d) {
    t->num_values[d] = nvals[d];
    t->minval[d] = minval[d];
    t->maxval[d] = maxval[d];
    t->delta[d] = delta[d];
  }
  memcpy(t->file, file, sizeof file);

  /* The cube is cut into ppd pieces along each axis, the last piece taking
   * what the equal ones leave.  A partition's block is created where the
   * partition lives and first touched by the task that reads its slab. */
  u64 base[SHEN_DIM], last[SHEN_DIM];
  for (int d = 0; d < SHEN_DIM; ++d) {
    base[d] = nvals[d] / ppd;
    last[d] = nvals[d] - base[d] * (ppd - 1);
  }
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  u64 off[SHEN_DIM], cnt[SHEN_DIM], held[SHEN_DIM];
  /* The names are kept here as well as in the table: the table is handed to
   * its readers below, and a released block's mapping is no longer this
   * task's to read the next name out of. */
  ocrGuid_t *part = malloc((size_t)live * sizeof(ocrGuid_t));
  if (part == NULL) {
    PRINTF("sheneos_hpx: out of memory\n");
    ocrShutdown();
    return NULL_GUID;
  }
  for (u64 z = 0; z < ppd; ++z) {
    for (u64 y = 0; y < ppd; ++y) {
      for (u64 x = 0; x < ppd; ++x) {
        u64 idx[SHEN_DIM] = {x, y, z};
        for (int d = 0; d < SHEN_DIM; ++d) {
          off[d] = base[d] * idx[d];
          cnt[d] = idx[d] + 1 == ppd ? last[d] : base[d];
          held[d] = shen_held(off[d], cnt[d], nvals[d]);
        }
        u64 k = x + (y + z * ppd) * ppd;
        ocrHint_t ph;
        mirror_rank_hint(&ph, mirror_owner(k, npart, nl), OCR_HINT_DB_T);
        void *addr;
        ocrDbCreate(&t->part[k], &addr, shen_part_bytes(held),
                    DB_PROP_NO_ACQUIRE, &ph, NO_ALLOC);
        part[k] = t->part[k];
      }
    }
  }
  ocrDbRelease(table);

  ocrGuid_t sum;
  ocrEdtCreate(&sum, sum_tpl, P_COUNT, pv, (u32)(ntotal + nl), NULL, EDT_PROP_NONE,
               &h0, NULL);
  pv[P_SUM] = mirror_guid_u64(sum);
  memcpy(params, pv, sizeof params);

  /* The workers start when every partition holds its slab, which is the join
   * the origin's driver performs before it spawns the first one. */
  ocrGuid_t ready = mirror_latch(live);
  ocrGuid_t start;
  ocrEdtCreate(&start, start_tpl, P_COUNT, pv, 2, NULL, EDT_PROP_NONE, &h0, NULL);
  ocrAddDependence(ready, start, 0, DB_MODE_NULL);
  ocrAddDependence(table, start, 1, DB_MODE_RO);

  for (u64 z = 0; z < ppd; ++z) {
    for (u64 y = 0; y < ppd; ++y) {
      for (u64 x = 0; x < ppd; ++x) {
        u64 idx[SHEN_DIM] = {x, y, z};
        for (int d = 0; d < SHEN_DIM; ++d) {
          off[d] = base[d] * idx[d];
          cnt[d] = idx[d] + 1 == ppd ? last[d] : base[d];
        }
        u64 k = x + (y + z * ppd) * ppd;
        params[P_OFF_YE] = off[0]; params[P_OFF_TEMP] = off[1];
        params[P_OFF_RHO] = off[2];
        params[P_CNT_YE] = cnt[0]; params[P_CNT_TEMP] = cnt[1];
        params[P_CNT_RHO] = cnt[2];
        ocrHint_t ih;
        mirror_rank_hint(&ih, mirror_owner(k, npart, nl), OCR_HINT_EDT_T);
        ocrGuid_t e, out;
        ocrEdtCreate(&e, init_tpl, P_COUNT, params, 2, NULL, EDT_PROP_NONE, &ih,
                     &out);
        ocrAddDependence(out, ready, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
        ocrAddDependence(table, e, 1, DB_MODE_RO);
        ocrAddDependence(part[k], e, 0, DB_MODE_RW);
      }
    }
  }
  free(part);
  return NULL_GUID;
}
