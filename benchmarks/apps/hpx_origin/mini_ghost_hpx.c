/* OCR mirror of the HPX port of Mantevo's MiniGhost: a 3D halo exchange on an
 * `npx x npy x npz` process grid, one rank per grid position, `num_vars`
 * variables per rank, each two grids of `(nx+2)(ny+2)(nz+2)` doubles advanced
 * `num_tsteps` steps.  Per step and variable a rank unpacks the six faces its
 * neighbours pushed into its halo, computes its interior in
 * `nx_block x ny_block x nz_block` chunks, packs the six faces its neighbours
 * will read, and -- where the variable is summed -- sends one partial to every
 * rank and compares the total against the total it injected. */
#include "hpx_mirror.h"

#include <math.h>

/* The origin's direction numbering, stencil codes and scaling modes. */
enum { NORTH, SOUTH, EAST, WEST, BACK, FRONT, NUM_NEIGHBORS };
enum { STENCIL_NONE = 20, STENCIL_2D5PT = 21, STENCIL_2D9PT = 22,
       STENCIL_3D7PT = 23, STENCIL_3D27PT = 24 };
enum { SCALING_STRONG = 1, SCALING_WEAK = 2 };

#define NO_RANK ((u64)-1)

/* Six kinds of rendezvous point.  A packed face carries one step's zone to the
 * rank that unpacks it; a partial carries one rank's sum to one other rank; a
 * packed zone's completion carries to the unpacks of the next step, on the rank
 * that packed it, that the plane the pack reads has been read -- a packed plane
 * is a full padded plane and an unpacked one is too, so the two meet on the
 * box's edge lines; a chunk's completion carries the ordering the next step's
 * chunks read -- only a variable that is NOT summed needs it, because a summed
 * variable's step is joined by its own sum; a step's completion carries what the
 * next step's chunks wait for beyond their own halo, which is the reported
 * all-reduce of a summed variable and the flux of one that is not; and a rank's
 * last step carries its completion to the rank that reports.  A face, a partial
 * and a completion have one producer and one consumer, which destroys them.  A
 * packed zone's completion has one producer and one consumer per receiving
 * direction of the next step, all on its own rank; the one for its own direction
 * destroys it.  A chunk's completion has one producer and up to seven consumers,
 * all on its own rank; the one at the same chunk index destroys it, which is a
 * task that waited on it and so runs after it was raised and after every other
 * consumer was registered.  Nothing else may free it: the spawner chain is not
 * ordered behind the chunks, so a task that merely ran later can be ahead of the
 * producer.  A step's completion has one producer and one consumer per chunk of
 * the next step, all on its own rank; on the summed arm the next step's join --
 * which waits for every one of them -- destroys it, and on the unsummed arm the
 * next step's first chunk does. */
enum { KIND_FACE, KIND_PARTIAL, KIND_PACKED, KIND_CHUNK, KIND_STEP, KIND_DONE,
       KIND_COUNT };

enum { P_NX, P_NY, P_NZ, P_NXB, P_NYB, P_NZB, P_NVARS, P_NSTEPS, P_STENCIL,
       P_PCTSUM, P_NSPIKES, P_NPX, P_NPY, P_NPZ, P_ETOL, P_SCALING,
       P_NL, P_RANK, P_RANGE,
       P_INIT_TPL, P_SPAWN_TPL, P_UNPACK_TPL, P_FLUX_TPL, P_CHUNK_TPL,
       P_PACK_TPL, P_SUM_TPL, P_CHECK_TPL, P_ADV_TPL, P_DONE_TPL, P_FINAL_TPL,
       P_T, P_V, P_IDX, P_GRID0, P_GRID1, P_SOURCE, P_FLUX, P_DONE, P_ERRMAX, P_START, P_INIT_GATE, P_ACC0, P_ACC1, P_SUM_OPEN, P_SUM_READY, P_SUM_LATCH,
       P_ARRIVE_TPL, P_REDUCED_TPL, P_START_TPL, P_CONSTRUCTED,
       P_REAP_TPL, P_REAP, P_COUNT };

/* ---------------------------------------------------------------- geometry */

/* The local dimension of a global one under the origin's two scaling modes.
 * The remainder is spread over the first ranks by LINEAR rank, which is what
 * the reference the port follows does. */
static inline u64 dim_of(u64 global, u64 np, u64 rank, u64 scaling) {
  if (scaling == SCALING_WEAK) return global;
  u64 n = global / np;
  return rank < global % np ? n + 1 : n;
}

static inline void coords(u64 rank, u64 npx, u64 npy, u64 *px, u64 *py, u64 *pz) {
  u64 xy = rank % (npx * npy);
  *px = xy % npx;
  *py = xy / npx;
  *pz = rank / (npx * npy);
}

/* The rank on the other side of one face, or NO_RANK at the edge of the
 * process grid -- where the origin leaves the send buffer without a
 * destination and the receive buffer invalid. */
static u64 neighbour(u64 *pv, u64 dir) {
  u64 rank = pv[P_RANK], npx = pv[P_NPX], npy = pv[P_NPY], npz = pv[P_NPZ];
  u64 px, py, pz;
  coords(rank, npx, npy, &px, &py, &pz);
  switch (dir) {
    case NORTH: return py + 1 != npy ? rank + npx : NO_RANK;
    case SOUTH: return py != 0 ? rank - npx : NO_RANK;
    case EAST:  return px + 1 != npx ? rank + 1 : NO_RANK;
    case WEST:  return px != 0 ? rank - 1 : NO_RANK;
    case FRONT: return pz + 1 != npz ? rank + npx * npy : NO_RANK;
    default:    return pz != 0 ? rank - npx * npy : NO_RANK;   /* BACK */
  }
}

/* A zone packed for one direction is unpacked as the opposite one: the north
 * neighbour stores it in its south halo. */
static inline u64 opposite(u64 dir) {
  switch (dir) {
    case NORTH: return SOUTH;
    case SOUTH: return NORTH;
    case EAST:  return WEST;
    case WEST:  return EAST;
    case BACK:  return FRONT;
    default:    return BACK;
  }
}

static inline u64 face_len(u64 dir, u64 gx, u64 gy, u64 gz) {
  switch (dir) {
    case NORTH: case SOUTH: return gx * gz;
    case EAST:  case WEST:  return gy * gz;
    default:                return gx * gy;
  }
}

static inline u64 gidx(u64 x, u64 y, u64 z, u64 gx, u64 gy) {
  return x + y * gx + z * gx * gy;
}

/* One rank's local interior dimensions. */
static void local_dims(u64 *pv, u64 *nx, u64 *ny, u64 *nz) {
  u64 rank = pv[P_RANK], sc = pv[P_SCALING];
  *nx = dim_of(pv[P_NX], pv[P_NPX], rank, sc);
  *ny = dim_of(pv[P_NY], pv[P_NPY], rank, sc);
  *nz = dim_of(pv[P_NZ], pv[P_NPZ], rank, sc);
}

/* How many chunks the interior is cut into, in each direction and in total. */
static void blocking(u64 *pv, u64 *bx, u64 *by, u64 *bz) {
  u64 nx, ny, nz;
  local_dims(pv, &nx, &ny, &nz);
  *bx = nx / pv[P_NXB] + (nx % pv[P_NXB] ? 1 : 0);
  *by = ny / pv[P_NYB] + (ny % pv[P_NYB] ? 1 : 0);
  *bz = nz / pv[P_NZB] + (nz % pv[P_NZB] ? 1 : 0);
}

/* The origin sums a variable when every variable is summed, or -- for a
 * fraction -- when its index is a multiple of ten. */
static inline int summed(u64 *pv, u64 v) {
  if (pv[P_PCTSUM] == 100) return 1;
  return pv[P_PCTSUM] > 0 && (v % 10) == 0;
}

/* ------------------------------------------------------------------ points */

static inline ocrGuid_t point(u64 *pv, u64 unit, u64 kind, u64 consumer) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]), unit * KIND_COUNT + kind,
                     consumer, pv[P_NL]);
}
/* The face a rank unpacks at step `t` for variable `v` out of its `dir` halo. */
static inline ocrGuid_t face_point(u64 *pv, u64 t, u64 v, u64 dir, u64 consumer) {
  return point(pv, (t * pv[P_NVARS] + v) * NUM_NEIGHBORS + dir, KIND_FACE, consumer);
}
static inline ocrGuid_t partial_point(u64 *pv, u64 t, u64 v, u64 sender, u64 consumer) {
  return point(pv, (t * pv[P_NVARS] + v) * pv[P_NL] + sender, KIND_PARTIAL, consumer);
}
/* The zone this rank packed at step `t` for direction `dir`, out of the grid
 * the unpacks of `t + 1` write. */
static inline ocrGuid_t packed_point(u64 *pv, u64 t, u64 v, u64 dir) {
  return point(pv, (t * pv[P_NVARS] + v) * NUM_NEIGHBORS + dir, KIND_PACKED,
               pv[P_RANK]);
}
static inline ocrGuid_t chunk_point(u64 *pv, u64 t, u64 v, u64 c, u64 nchunks) {
  return point(pv, (t * pv[P_NVARS] + v) * nchunks + c, KIND_CHUNK, pv[P_RANK]);
}
/* The step whose all-reduce has been reported: what the next step's chunks of
 * a summed variable wait for. */
static inline ocrGuid_t step_point(u64 *pv, u64 t, u64 v) {
  return point(pv, t * pv[P_NVARS] + v, KIND_STEP, pv[P_RANK]);
}
static inline ocrGuid_t done_point(u64 *pv, u64 rank) {
  return point(pv, rank, KIND_DONE, 0);
}

/* A signal carries no block, so there is no release for it to follow. */
static void raise_point(ocrGuid_t p) {
  mirror_edge_open(p);
  ocrEventSatisfy(p, NULL_GUID);
}

/* --------------------------------------------------------------- the kernel */

static inline double divisor_of(u64 stencil) {
  switch (stencil) {
    case STENCIL_2D5PT: return 1.0 / 5.0;
    case STENCIL_2D9PT: return 1.0 / 9.0;
    case STENCIL_3D7PT: return 1.0 / 7.0;
    case STENCIL_3D27PT: return 1.0 / 27.0;
    default: return 1.0;
  }
}

/* The five stencils, in the origin's operand order and with the origin's own
 * divisors -- which are not the divisors above for two of them: the nine- and
 * seven-point forms divide by five.  That is the kernel as it is written. */
static void stencil_apply(u64 stencil, double *dst, const double *src,
                          u64 gx, u64 gy, u64 x0, u64 x1, u64 y0, u64 y1,
                          u64 z0, u64 z1) {
  for (u64 z = z0; z != z1; ++z) {
    for (u64 y = y0; y != y1; ++y) {
      for (u64 x = x0; x != x1; ++x) {
        u64 i = gidx(x, y, z, gx, gy);
        u64 sx = 1, sy = gx, sz = gx * gy;
        switch (stencil) {
          case STENCIL_NONE:
            dst[i] = src[i];
            break;
          case STENCIL_2D5PT:
            dst[i] = (src[i - sx] + src[i - sy] + src[i] + src[i + sx] + src[i + sy])
                   * (1.0 / 5.0);
            break;
          case STENCIL_2D9PT:
            dst[i] = (src[i - sx - sy] + src[i - sx] + src[i - sx + sy]
                    + src[i - sy] + src[i] + src[i + sy]
                    + src[i + sx - sy] + src[i + sx] + src[i + sx + sy])
                   * (1.0 / 5.0);
            break;
          case STENCIL_3D7PT:
            dst[i] = (src[i - sz] + src[i - sx] + src[i - sy] + src[i]
                    + src[i + sx] + src[i + sy] + src[i + sz])
                   * (1.0 / 5.0);
            break;
          default: {   /* STENCIL_3D27PT */
            /* The twenty-seven neighbours in the origin's own order, so the
             * sum associates exactly as its expression does. */
            double s = 0.0;
            for (u64 xx = x - 1; xx != x + 2; ++xx)
              for (u64 yy = y - 1; yy != y + 2; ++yy)
                for (u64 zz = z - 1; zz != z + 2; ++zz)
                  s += src[gidx(xx, yy, zz, gx, gy)];
            dst[i] = s * (1.0 / 27.0);
            break;
          }
        }
      }
    }
  }
}

/* The origin's flux: a rank on a global boundary adds that boundary's plane of
 * its source grid, times the stencil's divisor.  The plane it names is the
 * HALO plane, which no neighbour ever writes on a global boundary, so this is
 * zero at every geometry -- the program's own accounting, carried as it is. */
static double flux_accumulate(u64 *pv, const double *g, u64 gx, u64 gy, u64 gz) {
  u64 px, py, pz;
  coords(pv[P_RANK], pv[P_NPX], pv[P_NPY], &px, &py, &pz);
  double divisor = divisor_of(pv[P_STENCIL]), flux = 0.0;

  if (px == 0) {
    for (u64 z = 1; z < gz - 1; ++z)
      for (u64 y = 1; y < gy - 1; ++y) flux += g[gidx(0, y, z, gx, gy)] * divisor;
  } else if (px == pv[P_NPX] - 1) {
    for (u64 z = 1; z < gz - 1; ++z)
      for (u64 y = 1; y < gy - 1; ++y) flux += g[gidx(gx - 1, y, z, gx, gy)] * divisor;
  }
  if (py == 0) {
    for (u64 z = 1; z < gz - 1; ++z)
      for (u64 x = 1; x < gx - 1; ++x) flux += g[gidx(x, 0, z, gx, gy)] * divisor;
  } else if (py == pv[P_NPY] - 1) {
    for (u64 z = 1; z < gz - 1; ++z)
      for (u64 x = 1; x < gx - 1; ++x) flux += g[gidx(x, gy - 1, z, gx, gy)] * divisor;
  }
  if (pz == 0) {
    for (u64 y = 1; y < gy - 1; ++y)
      for (u64 x = 1; x < gx - 1; ++x) flux += g[gidx(x, y, 0, gx, gy)] * divisor;
  } else if (pz == pv[P_NPZ] - 1) {
    for (u64 y = 1; y < gy - 1; ++y)
      for (u64 x = 1; x < gx - 1; ++x) flux += g[gidx(x, y, gz - 1, gx, gy)] * divisor;
  }
  return flux;
}

static double sum_all_grid(const double *g, u64 gx, u64 gy, u64 gz) {
  double sum = 0.0;
  for (u64 z = 1; z < gz - 1; ++z)
    for (u64 y = 1; y < gy - 1; ++y)
      for (u64 x = 1; x < gx - 1; ++x) sum += g[gidx(x, y, z, gx, gy)];
  return sum;
}

/* The zone one direction packs out of a grid, and the zone it is unpacked
 * into: the origin's planes, in the origin's loop order. */
static void pack_zone(u64 dir, const double *g, double *buf, u64 gx, u64 gy, u64 gz) {
  u64 n = 0;
  switch (dir) {
    case NORTH: for (u64 z = 0; z != gz; ++z) for (u64 x = 0; x != gx; ++x)
                  buf[n++] = g[gidx(x, gy - 2, z, gx, gy)]; break;
    case SOUTH: for (u64 z = 0; z != gz; ++z) for (u64 x = 0; x != gx; ++x)
                  buf[n++] = g[gidx(x, 1, z, gx, gy)]; break;
    case EAST:  for (u64 z = 0; z != gz; ++z) for (u64 y = 0; y != gy; ++y)
                  buf[n++] = g[gidx(gx - 2, y, z, gx, gy)]; break;
    case WEST:  for (u64 z = 0; z != gz; ++z) for (u64 y = 0; y != gy; ++y)
                  buf[n++] = g[gidx(1, y, z, gx, gy)]; break;
    case BACK:  for (u64 y = 0; y != gy; ++y) for (u64 x = 0; x != gx; ++x)
                  buf[n++] = g[gidx(x, y, gz - 2, gx, gy)]; break;
    default:    for (u64 y = 0; y != gy; ++y) for (u64 x = 0; x != gx; ++x)
                  buf[n++] = g[gidx(x, y, 1, gx, gy)]; break;   /* FRONT */
  }
}

static void unpack_zone(u64 dir, double *g, const double *buf, u64 gx, u64 gy, u64 gz) {
  u64 n = 0;
  switch (dir) {
    case NORTH: for (u64 z = 0; z != gz; ++z) for (u64 x = 0; x != gx; ++x)
                  g[gidx(x, gy - 1, z, gx, gy)] = buf[n++]; break;
    case SOUTH: for (u64 z = 0; z != gz; ++z) for (u64 x = 0; x != gx; ++x)
                  g[gidx(x, 0, z, gx, gy)] = buf[n++]; break;
    case EAST:  for (u64 z = 0; z != gz; ++z) for (u64 y = 0; y != gy; ++y)
                  g[gidx(gx - 1, y, z, gx, gy)] = buf[n++]; break;
    case WEST:  for (u64 z = 0; z != gz; ++z) for (u64 y = 0; y != gy; ++y)
                  g[gidx(0, y, z, gx, gy)] = buf[n++]; break;
    case BACK:  for (u64 y = 0; y != gy; ++y) for (u64 x = 0; x != gx; ++x)
                  g[gidx(x, y, gz - 1, gx, gy)] = buf[n++]; break;
    default:    for (u64 y = 0; y != gy; ++y) for (u64 x = 0; x != gx; ++x)
                  g[gidx(x, y, 0, gx, gy)] = buf[n++]; break;   /* FRONT */
  }
}

/* -------------------------------------------------------------------- tasks */

/* One received zone, into this rank's halo.  The origin runs one of these per
 * direction and they overlap where two halo planes meet, which is left as it
 * is; the faces they read are disjoint blocks.  What one waits for beyond its
 * own zone is every pack of its own rank of the step before, whose plane the
 * halo it writes meets on the box's edge lines; the packed point of its own
 * direction is the one it frees. */
static ocrGuid_t unpack_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nx, ny, nz;
  local_dims(pv, &nx, &ny, &nz);
  unpack_zone(pv[P_IDX], depv[depc - 1].ptr, depv[0].ptr, nx + 2, ny + 2, nz + 2);
  ocrEventDestroy(face_point(pv, pv[P_T], pv[P_V], pv[P_IDX], pv[P_RANK]));
  ocrEventDestroy(packed_point(pv, pv[P_T] - 1, pv[P_V], pv[P_IDX]));
  ocrDbDestroy(depv[0].guid);
  return NULL_GUID;
}

/* A zone the program packs and posts for a step that never runs: the send on
 * the last step, which no unpack consumes.  The work and the message are the
 * program's own, so both happen; this is the consumer that retires what they
 * leave behind -- the face and the point it travelled on -- and it runs on the
 * rank that would have unpacked it, which is where the message lands. */
static ocrGuid_t reap_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrEventDestroy(face_point(pv, pv[P_T], pv[P_V], pv[P_IDX], pv[P_RANK]));
  ocrDbDestroy(depv[0].guid);
  return NULL_GUID;
}

/* The result belongs to this step; only a summed variable accumulates it. */
static ocrGuid_t flux_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nx, ny, nz;
  local_dims(pv, &nx, &ny, &nz);
  ocrHint_t h;
  mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t db;
  double *value;
  ocrDbCreate(&db, (void **)&value, sizeof(*value), DB_PROP_NONE, &h, NO_ALLOC);
  *value = flux_accumulate(pv, depv[depc - 1].ptr, nx + 2, ny + 2, nz + 2);
  ocrDbRelease(db);
  return db;
}

/* One chunk of the interior: the origin's stencil over one block range. */
static ocrGuid_t chunk_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nx, ny, nz, nbx, nby, nbz;
  local_dims(pv, &nx, &ny, &nz);
  blocking(pv, &nbx, &nby, &nbz);
  u64 c = pv[P_IDX];
  u64 bx = c % nbx, by = (c / nbx) % nby, bz = c / (nbx * nby);
  u64 x0 = 1 + bx * pv[P_NXB], x1 = x0 + pv[P_NXB];
  u64 y0 = 1 + by * pv[P_NYB], y1 = y0 + pv[P_NYB];
  u64 z0 = 1 + bz * pv[P_NZB], z1 = z0 + pv[P_NZB];
  if (x1 > nx + 1) x1 = nx + 1;
  if (y1 > ny + 1) y1 = ny + 1;
  if (z1 > nz + 1) z1 = nz + 1;

  stencil_apply(pv[P_STENCIL], depv[depc - 1].ptr, depv[depc - 2].ptr,
                nx + 2, ny + 2, x0, x1, y0, y1, z0, z1);
  if (!summed(pv, pv[P_V])) {
    u64 nchunks = nbx * nby * nbz;
    /* The point of this chunk's own previous generation is one this task
     * waited on, so it is satisfied and every other task that waits on it was
     * registered before any chunk of this step could run.  Destroying it here
     * is the consumer freeing what it consumed; no later task may free it on
     * the strength of having run afterwards, because nothing orders the spawner
     * chain behind the chunks. */
    if (pv[P_T] > 1) {
      ocrEventDestroy(chunk_point(pv, pv[P_T] - 1, pv[P_V], c, nchunks));
      /* The step point of a variable with no join has no join to free it, so
       * the first chunk of the step that reads it does: a consumer that waited
       * on it, and one that cannot run before every other consumer of it was
       * registered. */
      if (c == 0) ocrEventDestroy(step_point(pv, pv[P_T] - 1, pv[P_V]));
    }
    ocrDbRelease(depv[depc - 1].guid);
    ocrDbRelease(depv[depc - 2].guid);
    raise_point(chunk_point(pv, pv[P_T], pv[P_V], c, nchunks));
  }
  return NULL_GUID;
}

/* One zone packed out of the grid this step wrote and pushed to the rank that
 * will unpack it at the next step -- the origin's send buffer. */
static ocrGuid_t pack_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nx, ny, nz, dir = pv[P_IDX];
  local_dims(pv, &nx, &ny, &nz);
  u64 gx = nx + 2, gy = ny + 2, gz = nz + 2, len = face_len(dir, gx, gy, gz);
  u64 to = neighbour(pv, dir);

  ocrHint_t dh;
  mirror_here_hint(&dh, OCR_HINT_DB_T);
  ocrGuid_t db;
  double *buf;
  ocrDbCreate(&db, (void **)&buf, len * sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
  pack_zone(dir, depv[depc - 1].ptr, buf, gx, gy, gz);
  ocrDbRelease(db);

  /* The plane this task read is given up before the point that lets the next
   * step's unpacks write the same grid is raised, so no writer can be admitted
   * while the hold that read it is still open.  The last step's zone is packed
   * and posted like every other one, but no step follows to wait for it, so it
   * raises no point. */
  ocrDbRelease(depv[depc - 1].guid);
  ocrGuid_t p = face_point(pv, pv[P_T] + 1, pv[P_V], opposite(dir), to);
  mirror_edge_open(p);
  ocrEventSatisfy(p, db);
  if (pv[P_T] < pv[P_NSTEPS]) raise_point(packed_point(pv, pv[P_T], pv[P_V], dir));
  return NULL_GUID;
}

typedef struct {
  atomic_flag gate_mutex;
  atomic_flag value_mutex;
  double value;
} sum_state_t;

static void sum_lock(atomic_flag *lock) {
  while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {}
}

static ocrGuid_t arrive_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  sum_state_t *state = depv[1].ptr;
  double value = *(const double *)depv[0].ptr;
  ocrEventDestroy(partial_point(pv, pv[P_T], pv[P_V], pv[P_IDX], pv[P_RANK]));
  ocrDbDestroy(depv[0].guid);
  sum_lock(&state->gate_mutex);
  sum_lock(&state->value_mutex);
  state->value += value;
  atomic_flag_clear_explicit(&state->value_mutex, memory_order_release);
  atomic_flag_clear_explicit(&state->gate_mutex, memory_order_release);
  /* The arrival's part in the gate is its output event, raised only after its
   * hold on the state has been released: what the gate admits then never
   * overlaps a hold that is still writing, and what retires the state waits
   * behind the gate. */
  return NULL_GUID;
}

static ocrGuid_t reduced_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  sum_state_t *state = depv[1].ptr;
  ocrHint_t h;
  mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_DB_T);
  ocrGuid_t db;
  double *value;
  ocrDbCreate(&db, (void **)&value, sizeof(*value), DB_PROP_NONE, &h, NO_ALLOC);
  sum_lock(&state->value_mutex);
  *value = state->value;
  state->value = 0.0;
  atomic_flag_clear_explicit(&state->value_mutex, memory_order_release);
  ocrEventDestroy(mirror_u64_guid(pv[P_SUM_OPEN]));
  ocrDbRelease(db);
  return db;
}

static void prepare_sum(u64 *pv) {
  ocrGuid_t open, ready, reduced, out;
  ocrEventCreate(&open, OCR_EVENT_STICKY_T, EVT_PROP_NONE);
  ocrEventCreate(&ready, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
  pv[P_SUM_OPEN] = mirror_guid_u64(open);
  pv[P_SUM_READY] = mirror_guid_u64(ready);
  pv[P_SUM_LATCH] = mirror_guid_u64(mirror_latch(pv[P_NL]));
  ocrGuid_t state = mirror_u64_guid(pv[pv[P_T] % 2 ? P_ACC1 : P_ACC0]);
  ocrHint_t h;
  mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
  ocrEdtCreate(&reduced, mirror_u64_guid(pv[P_REDUCED_TPL]), P_COUNT, pv,
               2, NULL, EDT_PROP_NONE, &h, &out);
  ocrAddDependence(out, ready, 0, DB_MODE_NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_SUM_LATCH]), reduced, 0, DB_MODE_NULL);
  ocrAddDependence(state, reduced, 1, DB_MODE_RW);
  for (u64 sender = 0; sender < pv[P_NL]; ++sender) {
    u64 params[P_COUNT];
    memcpy(params, pv, sizeof params);
    params[P_IDX] = sender;
    ocrGuid_t arrival, arrived;
    ocrEdtCreate(&arrival, mirror_u64_guid(pv[P_ARRIVE_TPL]), P_COUNT, params,
                 pv[P_T] == 0 ? 4 : 3, NULL, EDT_PROP_NONE, &h, &arrived);
    ocrAddDependence(arrived, mirror_u64_guid(pv[P_SUM_LATCH]),
                     OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrGuid_t point = partial_point(pv, pv[P_T], pv[P_V], sender, pv[P_RANK]);
    mirror_edge_open(point);
    ocrAddDependence(point, arrival, 0, DB_MODE_RO);
    ocrAddDependence(state, arrival, 1, DB_MODE_RW);
    if (pv[P_T] == 0)
      ocrAddDependence(mirror_u64_guid(pv[P_CONSTRUCTED]), arrival, 3, DB_MODE_NULL);
    ocrAddDependence(open, arrival, 2, DB_MODE_NULL);
  }
}

static void open_next_step(u64 *pv, u64 t, u64 v);
static void retire_variable(u64 *pv);

/* The first half of the origin's sum task: the grid's sum plus the flux, sent
 * to every rank as one partial each.  The origin's own task blocks here on its
 * all-reduce, so the mirror's task ends here and the check below is the half
 * that runs after it.  Every block this task holds is released before the
 * publish that lets a later reader of it be created, so no reader can be
 * admitted while a hold on what it reads is still open. */
static ocrGuid_t sum_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nx, ny, nz, nl = pv[P_NL], rank = pv[P_RANK], t = pv[P_T], v = pv[P_V];
  local_dims(pv, &nx, &ny, &nz);
  u64 bx, by, bz;
  blocking(pv, &bx, &by, &bz);
  double sum = sum_all_grid(depv[depc - 2].ptr, nx + 2, ny + 2, nz + 2);
  double *cumulative = depv[depc - 1].ptr;
  *cumulative += *(const double *)depv[bx * by * bz].ptr;
  sum += *cumulative;
  ocrDbDestroy(depv[bx * by * bz].guid);
  ocrDbRelease(depv[depc - 1].guid);
  ocrDbRelease(depv[depc - 2].guid);
  /* Every chunk of this step waited on the step before it, so this is the
   * consumer side freeing what it consumed. */
  if (t > 1) ocrEventDestroy(step_point(pv, t - 1, v));
  ocrEventSatisfy(mirror_u64_guid(pv[P_SUM_OPEN]), NULL_GUID);

  ocrHint_t dh;
  mirror_here_hint(&dh, OCR_HINT_DB_T);
  for (u64 c = 0; c < nl; ++c) {
    ocrGuid_t db;
    double *p;
    ocrDbCreate(&db, (void **)&p, sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
    *p = sum;
    ocrDbRelease(db);
    ocrGuid_t pt = partial_point(pv, t, v, rank, c);
    mirror_edge_open(pt);
    ocrEventSatisfy(pt, db);
  }
  return NULL_GUID;
}

static ocrGuid_t check_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 t = pv[P_T], v = pv[P_V];
  double value = *(const double *)depv[0].ptr;
  ocrDbDestroy(depv[0].guid);
  ocrEventDestroy(mirror_u64_guid(pv[P_SUM_READY]));

  if (pv[P_RANK] == 0) {
    double total;
    memcpy(&total, &pv[P_SOURCE], sizeof total);
    _Atomic double *err_max = depv[1].ptr;
    double err = fabs(total - value) / total;
    for (double seen = atomic_load(err_max);
         seen < err && !atomic_compare_exchange_weak(err_max, &seen, err); ) {}
    ocrDbRelease(depv[1].guid);
    double tol;
    memcpy(&tol, &pv[P_ETOL], sizeof tol);
    int over = err > tol;
    /* The error is reported when it is over the tolerance or when the step
     * falls on the report interval, which is the step count: the interval the
     * program takes when none is stated, and the only one it accepts. */
    if (over || t % pv[P_NSTEPS] == 0)
      PRINTF("Time step %llu for variable %llu the error is %g;"
             " error tolerance is %g.\n", (unsigned long long)t,
             (unsigned long long)v, err, tol);
    /* A run whose conservation check failed ends here and reports no scalar:
     * there is no result to report. */
    if (over) {
      ocrShutdown();
      return NULL_GUID;
    }
    PRINTF("sum_grid at step %llu\n", (unsigned long long)t);
  }
  /* The reported step is what the next step's chunks wait for.  The last step
   * has none, and its variable's state has no further reader. */
  if (t < pv[P_NSTEPS]) raise_point(step_point(pv, t, v));
  else retire_variable(pv);
  return NULL_GUID;
}

/* The join of a variable that is not summed: the origin gives such a
 * variable's next step the step's flux future in place of a sum, so the
 * ordering its chunks inherit is the flux and their own previous generation,
 * and nothing else -- which is what this step point carries. */
static ocrGuid_t adv_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 t = pv[P_T], v = pv[P_V];
  ocrDbDestroy(depv[0].guid);
  if (t < pv[P_NSTEPS]) {
    raise_point(step_point(pv, t, v));
    return NULL_GUID;
  }
  /* The last step's chunk points are consumed here -- no step follows to read
   * them -- so this task frees them, as every other consumer of one frees the
   * generation it read. */
  u64 nbx, nby, nbz;
  blocking(pv, &nbx, &nby, &nbz);
  u64 nchunks = nbx * nby * nbz;
  for (u64 c = 0; c < nchunks; ++c)
    ocrEventDestroy(chunk_point(pv, t, v, c, nchunks));
  retire_variable(pv);
  return NULL_GUID;
}

/* A rank's variable has run its last step. */
static ocrGuid_t done_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  raise_point(done_point(pv, pv[P_RANK]));
  return NULL_GUID;
}

/* The end, behind every rank's last step, as the origin's second barrier is. */
static ocrGuid_t final_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  for (u64 q = 0; q < pv[P_NL]; ++q) ocrEventDestroy(done_point(pv, q));
  mirror_app_e2e(pv[P_START]);
  _Atomic double *err_max = depv[pv[P_NL]].ptr;
  PRINTF("ERRMAX %.6e\n", atomic_load(err_max));
  ocrDbDestroy(depv[pv[P_NL]].guid);
  return NULL_GUID;
}

/* The report is what the run is for, so it is not held behind anything that
 * only tidies up: the posted zones no step consumes are retired in parallel
 * with it, and the runtime is told to stop when the report and the last of
 * them are both done. */
static ocrGuid_t close_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc; (void)depv;
  ocrShutdown();
  return NULL_GUID;
}

/* One step of one variable on one rank: the unpacks, the flux, the chunks, the
 * packs, and the task that opens what follows.  Every consumer of an output
 * event is registered before the task that raises it can run, which is why
 * each task's own dependences are added only once its completion has been
 * counted everywhere it is needed. */
static ocrGuid_t spawn_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 t = pv[P_T], v = pv[P_V], rank = pv[P_RANK];
  u64 nx, ny, nz, nbx, nby, nbz;
  local_dims(pv, &nx, &ny, &nz);
  blocking(pv, &nbx, &nby, &nbz);
  u64 nchunks = nbx * nby * nbz;
  u64 src = (t - 1) % 2, dst = t % 2;
  int is_summed = summed(pv, v);

  ocrHint_t h;
  mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  if (is_summed) prepare_sum(params);

  /* Which directions carry a zone this step, and which carry one onward: a
   * rank receives through the same face it sends through, which is where the
   * origin marks the receive buffer valid.  The first step receives nothing,
   * as the origin's own guard has it.  Every step sends, the last one
   * included: the program packs a zone and posts it for a step that never
   * runs, and that work and that message are the program's, so they happen
   * here too -- with a consumer whose only purpose is to retire what they
   * leave, since a rendezvous with no consumer is a point and a block nobody
   * frees. */
  u64 recv[NUM_NEIGHBORS], send[NUM_NEIGHBORS], nrecv = 0, nsend = 0;
  int last = t == pv[P_NSTEPS];
  for (u64 d = 0; d < NUM_NEIGHBORS; ++d) {
    u64 adjacent = neighbour(pv, d) != NO_RANK ? 1 : 0;
    send[d] = adjacent;
    recv[d] = (t > 1 && adjacent) ? 1 : 0;
    nrecv += recv[d];
    nsend += send[d];
  }

  /* The task that joins this step exists first, so the tasks it counts can be
   * registered on it as they are created.  It names what the origin's own join
   * names -- this step's chunks and its flux -- and, on the last step only, the
   * step's packs as well: what runs behind that join retires the grids, and a
   * block is destroyed only once nothing holds it.  On every other step the
   * packs are none of the join's business; what orders a pack against the next
   * step's unpacks is the point the pack raises for them. */
  u64 njoin_packs = last ? nsend : 0;
  u32 pack_base;
  ocrGuid_t after;
  if (is_summed) {
    pack_base = (u32)(nchunks + 1);
    ocrEdtCreate(&after, mirror_u64_guid(pv[P_SUM_TPL]), P_COUNT, params,
                 (u32)(nchunks + 3 + njoin_packs), NULL, EDT_PROP_NONE, &h, NULL);
  } else {
    /* A variable with no join of its own reports its step through the flux
     * alone, as the origin's own chain has it -- except on its last step, which
     * waits for that step's chunks too, the way the origin's run joins its
     * calculation futures before the variable is done. */
    ocrGuid_t adv_out = NULL_GUID;
    pack_base = (u32)(1 + (last ? nchunks : 0));
    ocrEdtCreate(&after, mirror_u64_guid(pv[P_ADV_TPL]), P_COUNT, params,
                 (u32)(pack_base + njoin_packs), NULL, EDT_PROP_NONE, &h,
                 last ? &adv_out : NULL);
    if (last)
      ocrAddDependence(adv_out, mirror_u64_guid(pv[P_DONE]), OCR_EVENT_LATCH_DECR_SLOT,
                       DB_MODE_NULL);
  }
  u32 pack_slot = 0;

  /* The flux reads the source grid once its halo is in place. */
  ocrGuid_t flux, flux_out;
  ocrEdtCreate(&flux, mirror_u64_guid(pv[P_FLUX_TPL]), P_COUNT, params,
               (u32)(nrecv + 1), NULL, EDT_PROP_NONE, &h, &flux_out);
  ocrAddDependence(flux_out, after, is_summed ? (u32)nchunks : 0, DB_MODE_RO);

  /* The chunks, each with the packs that read its result and the join that
   * follows the step registered on it before it can run. */
  ocrGuid_t chunk[nchunks], chunk_out[nchunks];
  u64 slots[nchunks];
  for (u64 c = 0; c < nchunks; ++c) {
    u64 bx = c % nbx, by = (c / nbx) % nby, bz = c / (nbx * nby);
    u64 waits = 0;
    if (bx == 0) waits += recv[WEST];
    if (bx + 1 == nbx) waits += recv[EAST];
    if (by == 0) waits += recv[SOUTH];
    if (by + 1 == nby) waits += recv[NORTH];
    if (bz == 0) waits += recv[FRONT];
    if (bz + 1 == nbz) waits += recv[BACK];
    /* What a chunk waits for beyond its halo is the step before it: the
     * reported all-reduce of a summed variable, or the flux of one that is not,
     * which is the future the origin hands such a variable's next step.  For a
     * summed variable the previous generation of the chunks comes with that one
     * edge, because the join that reports the step waits for all of them; a
     * variable that is not summed has no such join, so its chunks name the
     * previous generation themselves, as the origin's chunk futures do. */
    if (t > 1) {
      waits += 1;
      if (!is_summed) {
        waits += 1;
        if (bx > 0) ++waits;
        if (bx + 1 < nbx) ++waits;
        if (by > 0) ++waits;
        if (by + 1 < nby) ++waits;
        if (bz > 0) ++waits;
        if (bz + 1 < nbz) ++waits;
      }
    }
    slots[c] = waits;
    params[P_IDX] = c;
    ocrEdtCreate(&chunk[c], mirror_u64_guid(pv[P_CHUNK_TPL]), P_COUNT, params,
                 (u32)(waits + 2), NULL, EDT_PROP_NONE, &h, &chunk_out[c]);
    if (is_summed) ocrAddDependence(chunk_out[c], after, (u32)c, DB_MODE_NULL);
  }

  /* The packs: each direction waits for the chunks that write the plane it
   * packs -- the low plane of an axis for the low direction, the high plane
   * for the high one. */
  ocrGuid_t pack[NUM_NEIGHBORS], reap[NUM_NEIGHBORS];
  u64 packed[NUM_NEIGHBORS];
  for (u64 d = 0; d < NUM_NEIGHBORS; ++d) {
    packed[d] = 0;
    if (!send[d]) continue;
    for (u64 c = 0; c < nchunks; ++c) {
      u64 bx = c % nbx, by = (c / nbx) % nby, bz = c / (nbx * nby);
      int gate = (d == WEST  && bx == 0) || (d == EAST  && bx + 1 == nbx)
              || (d == SOUTH && by == 0) || (d == NORTH && by + 1 == nby)
              || (d == FRONT && bz == 0) || (d == BACK  && bz + 1 == nbz);
      if (gate) ++packed[d];
    }
    params[P_IDX] = d;
    ocrGuid_t pack_out = NULL_GUID;
    ocrEdtCreate(&pack[d], mirror_u64_guid(pv[P_PACK_TPL]), P_COUNT, params,
                 (u32)(packed[d] + 1), NULL, EDT_PROP_NONE, &h,
                 last ? &pack_out : NULL);
    if (last) ocrAddDependence(pack_out, after, pack_base + pack_slot++, DB_MODE_NULL);
    u32 slot = 0;
    for (u64 c = 0; c < nchunks; ++c) {
      u64 bx = c % nbx, by = (c / nbx) % nby, bz = c / (nbx * nby);
      int gate = (d == WEST  && bx == 0) || (d == EAST  && bx + 1 == nbx)
              || (d == SOUTH && by == 0) || (d == NORTH && by + 1 == nby)
              || (d == FRONT && bz == 0) || (d == BACK  && bz + 1 == nbz);
      if (gate) ocrAddDependence(chunk_out[c], pack[d], slot++, DB_MODE_NULL);
    }
    /* The zone a last step posts has no step to unpack it, so it is given the
     * task that retires it, on the rank the zone is addressed to.  Its
     * completion is counted before its own dependence exists, and it is
     * counted where the runtime is told to stop: the retirement is not part of
     * what the run reports, but it must have happened before the end. */
    if (last) {
      u64 to = neighbour(pv, d);
      u64 rp[P_COUNT];
      memcpy(rp, params, sizeof rp);
      rp[P_T] = t + 1;
      rp[P_V] = v;
      rp[P_IDX] = opposite(d);
      rp[P_RANK] = to;
      ocrHint_t rh;
      mirror_rank_hint(&rh, to, OCR_HINT_EDT_T);
      ocrGuid_t reap_out;
      ocrEdtCreate(&reap[d], mirror_u64_guid(pv[P_REAP_TPL]), P_COUNT, rp, 1,
                   NULL, EDT_PROP_NONE, &rh, &reap_out);
      ocrAddDependence(reap_out, mirror_u64_guid(pv[P_REAP]),
                       OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    }
  }

  /* The unpacks, and the tasks that read what they write, registered before any
   * of them can run.  An unpack waits for the zone itself -- the neighbour's
   * pack of the step before -- and for every pack of its own rank of that step:
   * a packed plane and an unpacked one are both full padded planes of the same
   * grid, so the two meet on the box's edge lines wherever a rank has
   * neighbours on more than one axis.  A rank receives through the same faces it
   * sends through, so the directions that carry a zone into this step are
   * exactly the ones that carried one out of the step before. */
  ocrGuid_t unpack[NUM_NEIGHBORS];
  u32 flux_slot = 0;
  u32 chunk_slot[nchunks];
  for (u64 c = 0; c < nchunks; ++c) chunk_slot[c] = 0;
  for (u64 d = 0; d < NUM_NEIGHBORS; ++d) {
    if (!recv[d]) continue;
    params[P_IDX] = d;
    ocrGuid_t out;
    ocrEdtCreate(&unpack[d], mirror_u64_guid(pv[P_UNPACK_TPL]), P_COUNT, params,
                 (u32)(nrecv + 2), NULL, EDT_PROP_NONE, &h, &out);
    ocrAddDependence(out, flux, flux_slot++, DB_MODE_NULL);
    for (u64 c = 0; c < nchunks; ++c) {
      u64 bx = c % nbx, by = (c / nbx) % nby, bz = c / (nbx * nby);
      int reads = (d == WEST  && bx == 0)       || (d == EAST  && bx + 1 == nbx)
               || (d == SOUTH && by == 0)       || (d == NORTH && by + 1 == nby)
               || (d == FRONT && bz == 0)       || (d == BACK  && bz + 1 == nbz);
      if (reads) ocrAddDependence(out, chunk[c], chunk_slot[c]++, DB_MODE_NULL);
    }
    u32 slot = 1;
    for (u64 e = 0; e < NUM_NEIGHBORS; ++e) {
      if (!recv[e]) continue;
      ocrGuid_t p = packed_point(pv, t - 1, v, e);
      mirror_edge_open(p);
      ocrAddDependence(p, unpack[d], slot++, DB_MODE_NULL);
    }
  }

  /* The last step of an unsummed variable is joined before the variable
   * reports done, as the origin's run joins its calculation futures.  The
   * join counts the chunks' output events, which postdate their releases, so
   * the grids it retires have no holder left; the step's chunk points, which
   * no later step consumes, are what that join then frees. */
  if (!is_summed && last)
    for (u64 c = 0; c < nchunks; ++c)
      ocrAddDependence(chunk_out[c], after, (u32)(1 + c), DB_MODE_NULL);

  /* The step before this one: what it reported, which is the one dependence the
   * origin gives every chunk beyond its halo and its own neighbourhood. */
  if (t > 1) {
    ocrGuid_t p = step_point(pv, t - 1, v);
    mirror_edge_open(p);
    for (u64 c = 0; c < nchunks; ++c)
      ocrAddDependence(p, chunk[c], chunk_slot[c]++, DB_MODE_NULL);
  }

  /* The previous step's chunks, for a variable with no join of its own. */
  if (!is_summed && t > 1) {
    for (u64 c = 0; c < nchunks; ++c) {
      u64 bx = c % nbx, by = (c / nbx) % nby, bz = c / (nbx * nby);
      u64 want[7], n = 0;
      want[n++] = c;
      if (bx > 0) want[n++] = c - 1;
      if (bx + 1 < nbx) want[n++] = c + 1;
      if (by > 0) want[n++] = c - nbx;
      if (by + 1 < nby) want[n++] = c + nbx;
      if (bz > 0) want[n++] = c - nbx * nby;
      if (bz + 1 < nbz) want[n++] = c + nbx * nby;
      for (u64 i = 0; i < n; ++i) {
        ocrGuid_t p = chunk_point(pv, t - 1, v, want[i], nchunks);
        mirror_edge_open(p);
        ocrAddDependence(p, chunk[c], chunk_slot[c]++, DB_MODE_NULL);
      }
    }
  }

  /* Now the blocks, last, so that no task above could have run before its
   * completion was counted where it is needed. */
  ocrGuid_t gsrc = mirror_u64_guid(pv[src ? P_GRID1 : P_GRID0]);
  ocrGuid_t gdst = mirror_u64_guid(pv[dst ? P_GRID1 : P_GRID0]);
  for (u64 d = 0; d < NUM_NEIGHBORS; ++d) {
    if (!recv[d]) continue;
    ocrGuid_t p = face_point(pv, t, v, d, rank);
    mirror_edge_open(p);
    ocrAddDependence(p, unpack[d], 0, DB_MODE_RO);
    ocrAddDependence(gsrc, unpack[d], (u32)(nrecv + 1), DB_MODE_RW);
  }
  ocrAddDependence(gsrc, flux, flux_slot, DB_MODE_RO);
  for (u64 c = 0; c < nchunks; ++c) {
    ocrAddDependence(gsrc, chunk[c], (u32)slots[c], DB_MODE_RO);
    ocrAddDependence(gdst, chunk[c], (u32)slots[c] + 1, DB_MODE_RW);
  }
  for (u64 d = 0; d < NUM_NEIGHBORS; ++d) {
    if (!send[d]) continue;
    ocrAddDependence(gdst, pack[d], (u32)packed[d], DB_MODE_RO);
    if (last) {
      ocrGuid_t p = face_point(pv, t + 1, v, opposite(d), neighbour(pv, d));
      mirror_edge_open(p);
      ocrAddDependence(p, reap[d], 0, DB_MODE_RO);
    }
  }
  if (is_summed) {
    ocrAddDependence(gdst, after, (u32)(nchunks + 1 + njoin_packs), DB_MODE_RO);
    ocrAddDependence(mirror_u64_guid(pv[P_FLUX]), after,
                     (u32)(nchunks + 2 + njoin_packs), DB_MODE_RW);
  }

  if (is_summed) {
    ocrGuid_t chk, chk_out = NULL_GUID;
    ocrEdtCreate(&chk, mirror_u64_guid(pv[P_CHECK_TPL]), P_COUNT, params,
                 (u32)(1 + (rank == 0)), NULL, EDT_PROP_NONE, &h,
                 t == pv[P_NSTEPS] ? &chk_out : NULL);
    if (t == pv[P_NSTEPS])
      ocrAddDependence(chk_out, mirror_u64_guid(pv[P_DONE]), OCR_EVENT_LATCH_DECR_SLOT,
                       DB_MODE_NULL);
    if (rank == 0)
      ocrAddDependence(mirror_u64_guid(pv[P_ERRMAX]), chk, 1, DB_MODE_RW);
    ocrAddDependence(mirror_u64_guid(params[P_SUM_READY]), chk, 0, DB_MODE_RO);
  }

  if (t < pv[P_NSTEPS]) open_next_step(pv, t, v);
  return NULL_GUID;
}

/* One step further.  The step that issues it has issued everything of its own
 * first and waits for none of it, which is where the origin's loop stands when
 * it opens its next iteration: that loop never blocks, so the whole run's work
 * is issued up front and every task waits only for what it reads. */
static void open_next_step(u64 *pv, u64 t, u64 v) {
  ocrHint_t h;
  mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_T] = t + 1;
  params[P_V] = v;
  ocrGuid_t e;
  ocrEdtCreate(&e, mirror_u64_guid(pv[P_SPAWN_TPL]), P_COUNT, params, 0, NULL,
               EDT_PROP_NONE, &h, NULL);
}

/* A variable's own state, once the last thing that reads it has run: the two
 * grids, the cumulative flux and the two parity slots of its collective.  The
 * last step reports the variable done through the output event of the task
 * that runs it, which the step's own spawner registered: only an output event
 * postdates a task's block releases. */
static void retire_variable(u64 *pv) {
  ocrDbDestroy(mirror_u64_guid(pv[P_GRID0]));
  ocrDbDestroy(mirror_u64_guid(pv[P_GRID1]));
  ocrDbDestroy(mirror_u64_guid(pv[P_FLUX]));
  ocrDbDestroy(mirror_u64_guid(pv[P_ACC0]));
  ocrDbDestroy(mirror_u64_guid(pv[P_ACC1]));
}

/* The parameter report, from the rank that reports it and inside the measured
 * window, where the origin makes its own call: after the variables' state is
 * built and before any step runs. */
static void print_header(u64 *pv, u64 num_sum_grid) {
  u64 nx, ny, nz;
  local_dims(pv, &nx, &ny, &nz);
  double etol;
  memcpy(&etol, &pv[P_ETOL], sizeof etol);
  PRINTF("\n"
         "======================================================================\n"
         "\n"
         "        Mantevo miniapp MiniGhost experiment\n"
         "        HPX port\n"
         "\n"
         "======================================================================\n"
         "\n");
  switch (pv[P_STENCIL]) {
    case STENCIL_NONE:
      PRINTF("No computation inserted\n"); break;
    case STENCIL_2D5PT:
      PRINTF("Computation: 5 pt difference stencil on a 2D grid (STENCIL_2D5PT)\n");
      break;
    case STENCIL_2D9PT:
      PRINTF("Computation: 9 pt difference stencil on a 2D grid (STENCIL_2D9PT)\n");
      break;
    case STENCIL_3D7PT:
      PRINTF("Computation: 7 pt difference stencil on a 3D grid (STENCIL_3D27PT)\n");
      break;
    default:
      PRINTF("Computation: 27 pt difference stencil on a 3D grid (STENCIL_3D27PT)\n");
      break;
  }
  PRINTF("\n");
  PRINTF("        Global Grid Dimension: %llu, %llu, %llu\n",
         (unsigned long long)(nx * pv[P_NPX]), (unsigned long long)(ny * pv[P_NPY]),
         (unsigned long long)(nz * pv[P_NPZ]));
  PRINTF("        Local Grid Dimension : %llu, %llu, %llu\n",
         (unsigned long long)nx, (unsigned long long)ny, (unsigned long long)nz);
  PRINTF("\n");
  PRINTF("Number of variables: %llu\n", (unsigned long long)pv[P_NVARS]);
  PRINTF("\n");
  /* The interval the program takes when none is stated is the step count. */
  PRINTF("Error reported every %llu time steps. Tolerance is %g\n",
         (unsigned long long)pv[P_NSTEPS], etol);
  PRINTF("Number of variables reduced each time step: %llu; requested %llu%%\n",
         (unsigned long long)num_sum_grid, (unsigned long long)pv[P_PCTSUM]);
  PRINTF("\n");
  PRINTF("        Time Steps: %llu\n", (unsigned long long)pv[P_NSTEPS]);
  PRINTF("        Task grid : %llu, %llu, %llu\n", (unsigned long long)pv[P_NPX],
         (unsigned long long)pv[P_NPY], (unsigned long long)pv[P_NPZ]);
  PRINTF("\n");
  if (pv[P_SCALING] == SCALING_WEAK) PRINTF("HPX version, weak scaling\n");
  else PRINTF("HPX version, strong scaling\n");
  if (pv[P_NL] == 1) PRINTF("1 process executing\n");
  else PRINTF("%llu processes executing\n", (unsigned long long)pv[P_NL]);
  PRINTF("\n");
  time_t now = time(NULL);
  struct tm parts;
  char when[64];
  localtime_r(&now, &parts);
  strftime(when, sizeof when, "%Y-%b-%d %H:%M:%S", &parts);
  PRINTF("Program execution date %s\n", when);
  PRINTF("\n");
}

static ocrGuid_t init_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrEventDestroy(mirror_u64_guid(pv[P_SUM_READY]));
  return depv[0].guid;
}

static ocrGuid_t start_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  const u64 (*initial)[P_COUNT] = depv[0].ptr;
  u64 nvars = pv[P_NVARS];
  ocrHint_t h;
  mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
  for (u64 v = 0; v < nvars; ++v) {
    u64 params[P_COUNT];
    memcpy(params, initial[v], sizeof params);
    double total = *(const double *)depv[1 + v].ptr, value;
    memcpy(&value, &params[P_IDX], sizeof value);
    u64 loc = params[P_T];
    if (loc != NO_RANK) {
      ((double *)depv[1 + nvars + 2 * v].ptr)[loc] = value;
      ((double *)depv[2 + nvars + 2 * v].ptr)[loc] = value;
    }
    if (pv[P_RANK] == 0) total += value;
    memcpy(&params[P_SOURCE], &total, sizeof total);
    params[P_T] = 1;
    ocrDbDestroy(depv[1 + v].guid);
    ocrDbRelease(depv[1 + nvars + 2 * v].guid);
    ocrDbRelease(depv[2 + nvars + 2 * v].guid);
    ocrGuid_t first;
    ocrEdtCreate(&first, mirror_u64_guid(pv[P_SPAWN_TPL]), P_COUNT, params,
                 0, NULL, EDT_PROP_NONE, &h, NULL);
  }
  ocrEventDestroy(mirror_u64_guid(pv[P_CONSTRUCTED]));
  ocrDbDestroy(depv[0].guid);
  return NULL_GUID;
}

/* One rank's whole program: its grids, its share of the initial totals, and
 * the first step of every variable. */
static ocrGuid_t driver_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 nvars = pv[P_NVARS], nl = pv[P_NL], rank = pv[P_RANK];
  u64 nx, ny, nz;
  local_dims(pv, &nx, &ny, &nz);
  u64 gx = nx + 2, gy = ny + 2, gz = nz + 2, cells = gx * gy * gz;
  u64 px, py, pz;
  coords(rank, pv[P_NPX], pv[P_NPY], &px, &py, &pz);

  ocrHint_t dh, eh;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  mirror_rank_hint(&eh, rank, OCR_HINT_EDT_T);

  ocrGuid_t grid[2 * nvars], flux[nvars], acc[2 * nvars];
  u64 initial[nvars][P_COUNT];
  ocrGuid_t constructed;
  ocrEventCreate(&constructed, OCR_EVENT_STICKY_T, EVT_PROP_NONE);
  pv[P_CONSTRUCTED] = mirror_guid_u64(constructed);
  pv[P_DONE] = mirror_guid_u64(mirror_latch(nvars));

  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  ocrGuid_t rep;
  ocrEdtCreate(&rep, mirror_u64_guid(pv[P_DONE_TPL]), P_COUNT, params, 1, NULL,
               EDT_PROP_NONE, &eh, NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_DONE]), rep, 0, DB_MODE_NULL);

  /* The origin's draws, in the origin's order: for every variable a spikes
   * object (its values, then its locations) and then that variable's grid.
   * Only the first spikes object is ever used, but every one of them is drawn,
   * so the stream reaches the grids where the origin leaves it. */
  u64 global_nx = nx * pv[P_NPX], global_ny = ny * pv[P_NPY], global_nz = nz * pv[P_NPZ];
  double global_n = (double)(global_nx * global_ny * global_nz);
  u64 nspikes = pv[P_NSPIKES];
  mirror_mt19937_t gen;
  mirror_mt_seed(&gen, 0);
  double *spike_val = malloc(nvars * nspikes * sizeof(double));
  if (spike_val == NULL) {
    PRINTF("mini_ghost_hpx: out of memory\n");
    ocrShutdown();
    return NULL_GUID;
  }

  for (u64 v = 0; v < nvars; ++v) {
    for (u64 i = 0; i < nvars * nspikes; ++i) {
      double d = mirror_mt_next(&gen) / 4294967296.0;
      if (v == 0) spike_val[i] = d * global_n;
    }
    if (v == 0) spike_val[0] = global_n;
    /* The spike locations are drawn and not kept: the one spike this program
     * places sits at the middle of the global grid, and the draws must still
     * be spent so the stream reaches the grid where the origin leaves it. */
    for (u64 i = 0; i < 3 * nspikes; ++i) mirror_mt_next(&gen);

    double *a, *b;
    ocrDbCreate(&grid[v * 2 + 0], (void **)&a, cells * sizeof(double),
                DB_PROP_NONE, &dh, NO_ALLOC);
    ocrDbCreate(&grid[v * 2 + 1], (void **)&b, cells * sizeof(double),
                DB_PROP_NONE, &dh, NO_ALLOC);
    memset(a, 0, cells * sizeof(double));
    memset(b, 0, cells * sizeof(double));
    double sum = 0.0;
    for (u64 z = 1; z != nz + 1; ++z)
      for (u64 y = 1; y != ny + 1; ++y)
        for (u64 x = 1; x != nx + 1; ++x) {
          double value = mirror_mt_next(&gen) / 4294967296.0;
          a[gidx(x, y, z, gx, gy)] = value;
          b[gidx(x, y, z, gx, gy)] = value;
          sum += value;
        }
    ocrDbRelease(grid[v * 2 + 0]);
    ocrDbRelease(grid[v * 2 + 1]);
    double *cumulative;
    ocrDbCreate(&flux[v], (void **)&cumulative, sizeof(*cumulative), DB_PROP_NONE, &dh, NO_ALLOC);
    *cumulative = 0.0;
    ocrDbRelease(flux[v]);

    for (u64 slot = 0; slot < 2; ++slot) {
      sum_state_t *state;
      ocrDbCreate(&acc[2 * v + slot], (void **)&state, sizeof(*state),
                   DB_PROP_NONE, &dh, NO_ALLOC);
      atomic_flag_clear(&state->gate_mutex);
      atomic_flag_clear(&state->value_mutex);
      state->value = 0.0;
      ocrDbRelease(acc[2 * v + slot]);
    }
    memcpy(initial[v], pv, sizeof initial[v]);
    initial[v][P_V] = v;
    initial[v][P_T] = 0;
    initial[v][P_ACC0] = mirror_guid_u64(acc[2 * v]);
    initial[v][P_ACC1] = mirror_guid_u64(acc[2 * v + 1]);
    prepare_sum(initial[v]);
    ocrEventSatisfy(mirror_u64_guid(initial[v][P_SUM_OPEN]), NULL_GUID);

    /* This rank's share of the variable's initial total, to every rank. */
    for (u64 c = 0; c < nl; ++c) {
      ocrGuid_t db;
      double *p;
      ocrDbCreate(&db, (void **)&p, sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
      *p = sum;
      ocrDbRelease(db);
      ocrGuid_t pt = partial_point(pv, 0, v, rank, c);
      mirror_edge_open(pt);
      ocrEventSatisfy(pt, db);
    }
  }

  ocrGuid_t start;
  ocrEdtCreate(&start, mirror_u64_guid(pv[P_START_TPL]), P_COUNT, pv,
               (u32)(3 * nvars + 2), NULL, EDT_PROP_NONE, &eh, NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_INIT_GATE]), start,
                   (u32)(3 * nvars + 1), DB_MODE_NULL);
  ocrGuid_t records_db;
  u64 (*records)[P_COUNT];
  ocrDbCreate(&records_db, (void **)&records, sizeof initial, DB_PROP_NONE, &dh, NO_ALLOC);

  /* Where this rank's spike lands, in the origin's arithmetic: at the middle
   * of the global grid, owned by the rank whose inclusive range holds it. */
  u64 first_nx = nx * px + 1, first_ny = ny * py + 1, first_nz = nz * pz + 1;
  for (u64 v = 0; v < nvars; ++v) {
    u64 xloc, yloc, zloc;
    if (nspikes != 0) {
      xloc = global_nx / 2;
      yloc = global_ny / 2;
      zloc = global_nz / 2;
    } else {
      xloc = yloc = zloc = 0;
    }
    u64 packed_loc = NO_RANK;
    if (nspikes != 0 && first_nx <= xloc && xloc <= first_nx + nx + 1
        && first_ny <= yloc && yloc <= first_ny + ny + 1
        && first_nz <= zloc && zloc <= first_nz + nz + 1)
      packed_loc = gidx(xloc - first_nx, yloc - first_ny, zloc - first_nz, gx, gy);

    memcpy(params, initial[v], sizeof params);
    params[P_GRID0] = mirror_guid_u64(grid[v * 2]);
    params[P_GRID1] = mirror_guid_u64(grid[v * 2 + 1]);
    params[P_FLUX] = mirror_guid_u64(flux[v]);
    params[P_T] = packed_loc;
    double value = nspikes != 0 ? spike_val[v] : 0.0;
    memcpy(&params[P_IDX], &value, sizeof value);
    memcpy(records[v], params, sizeof params);
    ocrGuid_t e, initialized;
    ocrEdtCreate(&e, mirror_u64_guid(pv[P_INIT_TPL]), P_COUNT, params,
                 1, NULL, EDT_PROP_NONE, &eh, &initialized);
    ocrAddDependence(initialized, mirror_u64_guid(pv[P_INIT_GATE]),
                     OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(initialized, start, (u32)(1 + v), DB_MODE_RO);
    ocrAddDependence(grid[v * 2], start, (u32)(1 + nvars + 2 * v), DB_MODE_RW);
    ocrAddDependence(grid[v * 2 + 1], start, (u32)(2 + nvars + 2 * v), DB_MODE_RW);
    ocrAddDependence(mirror_u64_guid(params[P_SUM_READY]), e, 0, DB_MODE_RO);
  }
  free(spike_val);
  ocrDbRelease(records_db);
  ocrAddDependence(records_db, start, 0, DB_MODE_RO);
  if (rank == 0) {
    u64 num_sum_grid = 0;
    for (u64 v = 0; v < nvars; ++v) num_sum_grid += summed(pv, v) ? 1 : 0;
    print_header(pv, num_sum_grid);
  }
  ocrEventSatisfy(constructed, NULL_GUID);
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 nx = 100, ny = 100, nz = 100, nxb = 100, nyb = 100, nzb = 10;
  u64 nvars = 5, nsteps = 100, stencil = 21, pctsum = 0, nspikes = 1;
  u64 npx = 1, npy = 1, npz = 1, scaling = SCALING_WEAK;
  double etol = 0.00001;
  int bad = mirror_option_u64(argdb, argc, "--scaling", &scaling) < 0
         || mirror_option_u64(argdb, argc, "--nx", &nx) < 0
         || mirror_option_u64(argdb, argc, "--ny", &ny) < 0
         || mirror_option_u64(argdb, argc, "--nz", &nz) < 0
         || mirror_option_u64(argdb, argc, "--nx_block", &nxb) < 0
         || mirror_option_u64(argdb, argc, "--ny_block", &nyb) < 0
         || mirror_option_u64(argdb, argc, "--nz_block", &nzb) < 0
         || mirror_option_u64(argdb, argc, "--num_vars", &nvars) < 0
         || mirror_option_u64(argdb, argc, "--num_tsteps", &nsteps) < 0
         || mirror_option_u64(argdb, argc, "--stencil", &stencil) < 0
         || mirror_option_u64(argdb, argc, "--percent_sum", &pctsum) < 0
         || mirror_option_u64(argdb, argc, "--num_spikes", &nspikes) < 0
         || mirror_option_u64(argdb, argc, "--npx", &npx) < 0
         || mirror_option_u64(argdb, argc, "--npy", &npy) < 0
         || mirror_option_u64(argdb, argc, "--npz", &npz) < 0
         || mirror_option_f64(argdb, argc, "--error_tol", &etol) < 0;

  /* One dimension for all three, which the origin reads in place of the three
   * separate ones whenever it is given at all.  (Its process-grid counterpart
   * is registered by the origin and never read, so naming that one changes
   * nothing here either.) */
  u64 ndim = 0;
  int has_ndim = mirror_option_u64(argdb, argc, "--ndim", &ndim);
  if (has_ndim < 0) bad = 1;
  if (has_ndim > 0) { nx = ndim; ny = ndim; nz = ndim; }

  /* Options the origin registers and this program does not implement: the
   * periodic diffusion report, checkpointing, the performance report, and the
   * grid debug mode -- which in the origin makes a different program, since
   * its own setup turns it into every variable summed and a grid written to
   * file.  Naming one is usage, so no run can differ from the origin in a way
   * nothing states. */
  char cpfile[4096];
  u64 refused_value = 0;
  int refused = mirror_option_u64(argdb, argc, "--report_diffusion", &refused_value) != 0
             || mirror_option_u64(argdb, argc, "--checkpoint_interval", &refused_value) != 0
             || mirror_option_u64(argdb, argc, "--report_perf", &refused_value) != 0
             || mirror_option_u64(argdb, argc, "--debug_grid", &refused_value) != 0
             || mirror_option_str(argdb, argc, "--checkpoint_file", cpfile, sizeof cpfile) != 0;

  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  /* The origin's own rule: a process grid nobody stated becomes a row. */
  if (nl > 1 && npx == 1 && npy == 1 && npz == 1) npx = nl;

  u64 lx = 0, ly = 0, lz = 0, nbx = 0, nby = 0, nbz = 0, points = 1;
  int sane = !bad && !refused && nl != 0 && nsteps >= 1 && npx >= 1 && npy >= 1 && npz >= 1
          && npx <= nl && npy <= nl && npz <= nl && npx * npy * npz == nl
          && pctsum <= 100 && nvars >= 1 && nspikes == 1
          && nxb >= 1 && nyb >= 1 && nzb >= 1
          && (scaling == SCALING_WEAK || scaling == SCALING_STRONG)
          && (stencil >= STENCIL_NONE && stencil <= STENCIL_3D27PT);
  if (sane) {
    /* Rank 0's dimensions bound every rank's: the remainder only ever adds
     * one. */
    lx = dim_of(nx, npx, 0, scaling);
    ly = dim_of(ny, npy, 0, scaling);
    lz = dim_of(nz, npz, 0, scaling);
    sane = lx >= 1 && ly >= 1 && lz >= 1;
    if (sane) {
      nbx = lx / nxb + (lx % nxb ? 1 : 0);
      nby = ly / nyb + (ly % nyb ? 1 : 0);
      nbz = lz / nzb + (lz % nzb ? 1 : 0);
      u64 cells = 1;
      sane = mirror_fits(&cells, lx + 2, (u64)1 << 30)
          && mirror_fits(&cells, ly + 2, (u64)1 << 30)
          && mirror_fits(&cells, lz + 2, (u64)1 << 30);
      /* A point's index is (unit * KIND_COUNT + kind) * nl + consumer, and a
       * unit is a (step, variable) pair times the widest fan any kind gives
       * such a pair: six faces or six packed zones, one partial per rank, or
       * one per chunk.  Rank zero's dimensions are the largest, since the
       * remainder goes to the first ranks, so its chunk count bounds every
       * rank's.  Steps run from one, and a face is named for the step that
       * reads it, so the pairs run one past the last step. */
      u64 chunks = 1, fan = NUM_NEIGHBORS, unit = nsteps + 2;
      sane = sane && mirror_fits(&chunks, nbx, 0xffffffffu)
                  && mirror_fits(&chunks, nby, 0xffffffffu)
                  && mirror_fits(&chunks, nbz, 0xffffffffu);
      if (chunks > fan) fan = chunks;
      if (nl > fan) fan = nl;
      sane = sane && mirror_fits(&unit, nvars, 0xffffffffu)
                  && mirror_fits(&unit, fan, 0xffffffffu);
      points = unit;
      sane = sane && mirror_fits(&points, KIND_COUNT, 0xffffffffu)
                  && mirror_fits(&points, nl, 0xffffffffu);
    }
  }

  /* A variable that is not summed has no join of its own, so the ordering its
   * next step needs rides the previous step's chunk completions -- and the
   * set of them a chunk waits for is the program's own: itself and its six
   * FACE-adjacent chunks.  The nine- and 27-point stencils read cells a
   * DIAGONALLY adjacent chunk writes, and that pair has no edge.  Giving the
   * mirror one would be an ordering the program does not have, so the
   * combination that would reach it is refused rather than run: an unsummed
   * variable, a stencil that reads diagonals, and a blocking that puts a
   * diagonal chunk on the axes it reads them from. */
  int unsummed = pctsum == 0 || (pctsum < 100 && nvars >= 2);
  int diagonal = stencil == STENCIL_2D9PT
                     ? (nbx >= 2 && nby >= 2)
                     : (stencil == STENCIL_3D27PT
                        && (nbx >= 2) + (nby >= 2) + (nbz >= 2) >= 2);
  if (sane && unsummed && diagonal) {
    PRINTF("mini_ghost_hpx: --percent_sum=%llu leaves a variable unsummed and"
           " --stencil=%llu reads a diagonally adjacent chunk, which that"
           " variable's chunk dependences -- itself and its six face-adjacent"
           " chunks, as the program states them -- do not order it against."
           " Run --percent_sum=100, a stencil in 20, 21 or 23, or a blocking"
           " with no diagonal chunk (%llu x %llu x %llu here).\n",
           (unsigned long long)pctsum, (unsigned long long)stencil,
           (unsigned long long)nbx, (unsigned long long)nby,
           (unsigned long long)nbz);
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t range = NULL_GUID;
  if (sane && ocrGuidRangeCreate(&range, points, GUID_USER_EVENT_STICKY) != 0)
    sane = 0;
  if (!sane) {
    PRINTF("mini_ghost_hpx: usage --scaling=S --nx=X --ny=Y --nz=Z [--ndim=N]"
           " --nx_block=BX --ny_block=BY --nz_block=BZ --num_vars=V"
           " --num_tsteps=T --stencil=C --percent_sum=P --num_spikes=K"
           " --npx=PX --npy=PY --npz=PZ --error_tol=E"
           " (S is 1 or 2, C in 20..24, P <= 100, T >= 1, V >= 1, K = 1 --"
           " the origin runs the whole simulation once per spike and this"
           " program runs one pass -- every block size >= 1, PX*PY*PZ equal"
           " to the rank count, a local dimension of at least one, and a name"
           " space below 2^32; N sets all three dimensions at once."
           " --report_diffusion, --checkpoint_interval, --checkpoint_file,"
           " --report_perf and --debug_grid name work this program does not"
           " implement and are refused rather than ignored)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t driver_tpl, init_tpl, spawn_tpl, unpack_tpl, flux_tpl, chunk_tpl;
  ocrGuid_t pack_tpl, sum_tpl, check_tpl, adv_tpl, done_tpl, final_tpl;
  ocrGuid_t arrive_tpl, reduced_tpl, start_tpl, reap_tpl, close_tpl;
  ocrEdtTemplateCreate(&reap_tpl, reap_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&close_tpl, close_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&start_tpl, start_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&arrive_tpl, arrive_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&reduced_tpl, reduced_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&driver_tpl, driver_edt, P_COUNT, 0);
  ocrEdtTemplateCreate(&init_tpl, init_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&spawn_tpl, spawn_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&unpack_tpl, unpack_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&flux_tpl, flux_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&chunk_tpl, chunk_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&pack_tpl, pack_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&sum_tpl, sum_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&check_tpl, check_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&adv_tpl, adv_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&done_tpl, done_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&final_tpl, final_edt, P_COUNT, EDT_PARAM_UNK);

  u64 pv[P_COUNT] = {0};
  pv[P_NX] = nx; pv[P_NY] = ny; pv[P_NZ] = nz;
  pv[P_NXB] = nxb; pv[P_NYB] = nyb; pv[P_NZB] = nzb;
  pv[P_NVARS] = nvars; pv[P_NSTEPS] = nsteps; pv[P_STENCIL] = stencil;
  pv[P_PCTSUM] = pctsum; pv[P_NSPIKES] = nspikes;
  pv[P_NPX] = npx; pv[P_NPY] = npy; pv[P_NPZ] = npz; pv[P_SCALING] = scaling;
  memcpy(&pv[P_ETOL], &etol, sizeof etol);
  pv[P_NL] = nl;
  pv[P_RANGE] = mirror_guid_u64(range);
  pv[P_START_TPL] = mirror_guid_u64(start_tpl);
  pv[P_ARRIVE_TPL] = mirror_guid_u64(arrive_tpl);
  pv[P_REDUCED_TPL] = mirror_guid_u64(reduced_tpl);
  pv[P_INIT_TPL] = mirror_guid_u64(init_tpl);
  pv[P_SPAWN_TPL] = mirror_guid_u64(spawn_tpl);
  pv[P_UNPACK_TPL] = mirror_guid_u64(unpack_tpl);
  pv[P_FLUX_TPL] = mirror_guid_u64(flux_tpl);
  pv[P_CHUNK_TPL] = mirror_guid_u64(chunk_tpl);
  pv[P_PACK_TPL] = mirror_guid_u64(pack_tpl);
  pv[P_SUM_TPL] = mirror_guid_u64(sum_tpl);
  pv[P_CHECK_TPL] = mirror_guid_u64(check_tpl);
  pv[P_ADV_TPL] = mirror_guid_u64(adv_tpl);
  pv[P_DONE_TPL] = mirror_guid_u64(done_tpl);
  pv[P_REAP_TPL] = mirror_guid_u64(reap_tpl);

  /* The end belongs to the rank that reports, and exists before any rank can
   * reach it. */
  ocrHint_t h0;
  mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrHint_t dh0;
  mirror_rank_hint(&dh0, 0, OCR_HINT_DB_T);
  ocrGuid_t err_db;
  _Atomic double *err_max;
  ocrDbCreate(&err_db, (void **)&err_max, sizeof(*err_max), DB_PROP_NONE, &dh0, NO_ALLOC);
  atomic_init(err_max, 0.0);
  ocrDbRelease(err_db);
  pv[P_ERRMAX] = mirror_guid_u64(err_db);
  pv[P_INIT_GATE] = mirror_guid_u64(mirror_latch(nvars * nl));

  /* Every rank packs and posts the zone of the step that never runs, once per
   * variable and per neighbour, and every one of those posts has a task that
   * retires it.  The count is twice the process grid's adjacent pairs -- each
   * pair is a neighbour to both of its sides -- times the variables, and one
   * more for the report itself, so that the counter is never created at zero
   * and the run is told to stop only behind both. */
  u64 faces = 2 * ((npx - 1) * npy * npz + npx * (npy - 1) * npz
                   + npx * npy * (npz - 1));
  pv[P_REAP] = mirror_guid_u64(mirror_latch(nvars * faces + 1));
  ocrGuid_t closer;
  ocrEdtCreate(&closer, close_tpl, P_COUNT, pv, 1, NULL, EDT_PROP_NONE, &h0, NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_REAP]), closer, 0, DB_MODE_NULL);

  pv[P_START] = mirror_now_ns();
  ocrGuid_t fin, fin_out;
  ocrEdtCreate(&fin, final_tpl, P_COUNT, pv, (u32)(nl + 1), NULL, EDT_PROP_NONE, &h0,
               &fin_out);
  ocrAddDependence(fin_out, mirror_u64_guid(pv[P_REAP]), OCR_EVENT_LATCH_DECR_SLOT,
                   DB_MODE_NULL);
  for (u64 q = 0; q < nl; ++q) {
    ocrGuid_t p = done_point(pv, q);
    mirror_edge_open(p);
    ocrAddDependence(p, fin, (u32)q, DB_MODE_NULL);
  }

  ocrAddDependence(err_db, fin, (u32)nl, DB_MODE_RO);
  mirror_spmd_fork(driver_tpl, pv, P_COUNT, P_RANK, nl, 0, NULL);
  return NULL_GUID;
}
