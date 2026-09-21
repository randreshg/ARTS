/* OCR mirror of hpx-fft's fft_hpx_loop: a distributed 2D real-to-complex FFT.
 * A dim_c_x x 2*dim_c_y real array is block-distributed by rows, n_x_local
 * rows per rank, every row the same ramp.  Each rank transforms its rows in
 * y, cuts each row into one outgoing chunk per rank, exchanges, transposes
 * the arrivals into its share of the transposed array, transforms those rows
 * in x, and sends everything back the same way.  Two exchanges, one message
 * per (source, destination) in each -- what one scatter_to costs. */
#include "hpx_mirror.h"

#include <fftw3.h>
#include "fftw_reloc/reloc.h"

enum { P_NL, P_RANK, P_NXL, P_NYL, P_CX, P_CY, P_RY, P_CYPART, P_CXPART,
       P_PLANFLAG, P_RANGE, P_SUM_EDT, P_PLAN_R2C, P_PLAN_C2C, P_IDX,
       P_FFT1_TPL, P_SPLIT1_TPL, P_XPOSE1_TPL, P_FFT2_TPL, P_SPLIT2_TPL,
       P_XPOSE2_TPL, P_REAP1_TPL, P_FINISH_TPL, P_PUBLISH_TPL, P_PHASE,
       P_PLAN_DB, P_PLAN_SIZE, P_SRC, P_XPOSE_SCOPE_TPL,
       P_XPOSE_OUTER_TPL, P_V_DB, P_W_DB, P_COUNT };

/* One rendezvous point per (phase, source, destination): the ordinal names
 * the phase and the source, the index the destination, so a point has one
 * producing task, one consuming rank and one destroyer, and no two units of
 * work share a name.  Several tasks of that rank read it -- every transpose
 * of the destination reads all of its arrivals -- which is why the destroyer
 * is a task of its own rather than the reader.  Two phases, hence 2*nl*nl
 * names in the reservation. */
static inline ocrGuid_t chunk_point(u64 *pv, u64 phase, u64 src, u64 dst) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]), phase * pv[P_NL] + src, dst,
                     pv[P_NL]);
}

/* One row's transform in y, in place: the origin's fft_1d_r2c_inplace, one
 * task per row as its parallel for_loop has it.  The row is a slice of the
 * rank's array: the task holds the array for write and names its own row by
 * offset, which is what lets sibling rows run at once on disjoint columns. */
static ocrGuid_t fft1_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  fftw_reloc_execute_r2c(fftw_reloc_image_at(depv[1].ptr), pv[P_PLAN_SIZE],
                         pv[P_PLAN_R2C], depv[0].ptr,
                         pv[P_IDX] * 2 * pv[P_CY] * sizeof(double));
  return NULL_GUID;
}

/* A row writes disjoint slices of every destination buffer. */
static ocrGuid_t split1_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 part = pv[P_CYPART], i = pv[P_IDX];
  const double *row = (const double *)depv[1].ptr + i * 2 * pv[P_CY];
  for (u64 j = 0; j < pv[P_NL]; ++j)
    memcpy((double *)depv[2 + j].ptr + i * part, row + j * part,
           part * sizeof(double));
  return NULL_GUID;
}

static ocrGuid_t publish_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  for (u32 j = 0; j + 1 < depc; ++j) {
    ocrGuid_t p = chunk_point(pv, pv[P_PHASE], pv[P_RANK], j);
    mirror_edge_open(p);
    ocrEventSatisfy(p, depv[1 + j].guid);
  }
  return NULL_GUID;
}

/* One (source, transposed row) pair retains the sequential input-row loop. */
static ocrGuid_t xpose1_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 part = pv[P_CYPART], k = pv[P_IDX], i = pv[P_SRC];
  u64 dim_input = pv[P_NXL] * part / part;
  const double *in = depv[0].ptr;
  double *w = (double *)depv[1].ptr + k * 2 * pv[P_CX];
  for (u64 j = 0; j < dim_input; ++j) {
    u64 index_in = part * j + 2 * k;
    u64 index_out = 2 * pv[P_NL] * j + 2 * i;
    w[index_out] = in[index_in];
    w[index_out + 1] = in[index_in + 1];
  }
  return NULL_GUID;
}

/* One transposed row's transform in x, in place: the origin's
 * fft_1d_c2c_inplace. */
static ocrGuid_t fft2_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  fftw_reloc_execute_c2c(fftw_reloc_image_at(depv[2].ptr), pv[P_PLAN_SIZE],
                         pv[P_PLAN_C2C], depv[1].ptr,
                         pv[P_IDX] * 2 * pv[P_CX] * sizeof(double));
  return NULL_GUID;
}

static ocrGuid_t split2_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 part = pv[P_CXPART], i = pv[P_IDX];
  const double *row = (const double *)depv[1].ptr + i * 2 * pv[P_CX];
  for (u64 j = 0; j < pv[P_NL]; ++j)
    memcpy((double *)depv[2 + j].ptr + i * part, row + j * part,
           part * sizeof(double));
  return NULL_GUID;
}

/* The input stride and sequential output-row loop follow the source arithmetic. */
static ocrGuid_t xpose2_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 part = pv[P_CYPART], j = pv[P_IDX], i = pv[P_SRC];
  u64 dim_input = pv[P_NYL] * pv[P_CXPART] / part;
  const double *in = depv[0].ptr;
  double *v = depv[1].ptr;
  u64 stride = 2 * pv[P_CY];
  for (u64 k = 0; k < dim_input; ++k) {
    u64 index_in = part * j + 2 * k;
    u64 index_out = 2 * pv[P_NL] * j + 2 * i;
    v[k * stride + index_out] = in[index_in];
    v[k * stride + index_out + 1] = in[index_in + 1];
  }
  return NULL_GUID;
}

/* The array a transpose writes is the phase's destination buffer: the
 * transposed rows in the first phase, the original rows in the second. */
static ocrGuid_t xpose_outer_edt(u32 paramc, u64 *pv, u32 depc,
                                ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 phase = pv[P_PHASE];
  ocrGuid_t array = mirror_u64_guid(pv[phase ? P_V_DB : P_W_DB]);
  ocrHint_t eh;
  mirror_rank_hint(&eh, pv[P_RANK], OCR_HINT_EDT_T);
  for (u64 j = 0; j < pv[P_NYL]; ++j) {
    ocrGuid_t e;
    pv[P_IDX] = j;
    ocrEdtCreate(&e, mirror_u64_guid(pv[phase ? P_XPOSE2_TPL : P_XPOSE1_TPL]),
                 P_COUNT, pv, 2, NULL, EDT_PROP_NONE, &eh, NULL);
    ocrAddDependence(depv[0].guid, e, 0, DB_MODE_RO);
    ocrAddDependence(array, e, 1, DB_MODE_RW);
  }
  return NULL_GUID;
}

/* The phase joins every incoming collective before starting either parallel loop. */
static ocrGuid_t xpose_scope_edt(u32 paramc, u64 *pv, u32 depc,
                                ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrHint_t eh;
  mirror_rank_hint(&eh, pv[P_RANK], OCR_HINT_EDT_T);
  for (u64 i = 0; i < pv[P_NL]; ++i) {
    ocrGuid_t e;
    pv[P_SRC] = i;
    ocrEdtCreate(&e, mirror_u64_guid(pv[P_XPOSE_OUTER_TPL]), P_COUNT, pv, 1,
                 NULL, EDT_PROP_FINISH, &eh, NULL);
    ocrAddDependence(depv[1 + i].guid, e, 0, DB_MODE_RO);
  }
  return NULL_GUID;
}

/* The first exchange's arrivals, once every transpose that reads them has
 * run.  A chunk has one producer and several readers, so no reader can be
 * its destroyer; this task depends on the same points and destroys both. */
static ocrGuid_t reap1_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nl = pv[P_NL], rank = pv[P_RANK];
  for (u64 q = 0; q < nl; ++q) {
    ocrDbDestroy(depv[1 + q].guid);
    ocrEventDestroy(chunk_point(pv, 0, q, rank));
  }
  return NULL_GUID;
}

/* A rank's whole ending: its share of the tally, handed to the task that
 * prints, and everything it still owns.  The sum is taken before anything is
 * destroyed, and the plans go last, as the origin's destructor has them. */
static ocrGuid_t finish_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nl = pv[P_NL], nxl = pv[P_NXL], cy = pv[P_CY];
  u64 rank = pv[P_RANK], slot_v = 2 + nl, slot_w = 3 + nl, slot_plan = 4 + nl;

  const double *v = depv[slot_v].ptr;
  double sum = 0.0;
  for (u64 c = 0; c < nxl * 2 * cy; ++c) sum += v[c];

  ocrHint_t dh;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  ocrGuid_t share;
  double *s;
  ocrDbCreate(&share, (void **)&s, sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
  *s = sum;
  ocrDbRelease(share);

  for (u64 q = 0; q < nl; ++q) {
    ocrDbDestroy(depv[2 + q].guid);
    ocrEventDestroy(chunk_point(pv, 1, q, rank));
  }
  ocrDbDestroy(depv[slot_v].guid);
  ocrDbDestroy(depv[slot_w].guid);
  void *image = fftw_reloc_image_at(depv[slot_plan].ptr);
  fftw_reloc_image_protect(image, pv[P_PLAN_SIZE], 0);
  fftw_reloc_destroy_pair(image, pv[P_PLAN_SIZE], pv[P_PLAN_R2C],
                            pv[P_PLAN_C2C]);
  ocrDbDestroy(depv[slot_plan].guid);
  /* The share goes in last: it is the slot the end task waits on, and the
   * planner teardown above is retained state this task must be done with
   * before the shutdown that push releases can begin. */
  ocrAddDependence(share, mirror_u64_guid(pv[P_SUM_EDT]), (u32)rank, DB_MODE_RO);
  return NULL_GUID;
}

/* The oracle, and the program's end: every rank's share, summed in rank
 * order.  It is created before the fork with one slot per rank, so shutdown
 * sits behind every rank's last work the way the origin's collective does. */
static ocrGuid_t sum_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv;
  double total = 0.0;
  for (u32 r = 0; r < depc; ++r) total += *(const double *)depv[r].ptr;
  PRINTF("CHECKSUM %.14e\n", total);
  for (u32 r = 0; r < depc; ++r) ocrDbDestroy(depv[r].guid);
  ocrShutdown();
  return NULL_GUID;
}

/* One rank's whole program.  Everything is created here, and in one order
 * that two OCR contracts fix: a join is once-type, destroyed the instant it
 * reaches zero, so every task that waits on a join is created and registered
 * before any task can count into it; and a task must not run before its own
 * output event has been counted into the join it belongs to, so each task's
 * dependences are added after that registration, the writable one last. */
static ocrGuid_t driver_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 nl = pv[P_NL], rank = pv[P_RANK], nxl = pv[P_NXL], nyl = pv[P_NYL];
  u64 cx = pv[P_CX], cy = pv[P_CY], ry = pv[P_RY];

  ocrHint_t dh, eh;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  mirror_rank_hint(&eh, rank, OCR_HINT_EDT_T);

  ocrGuid_t *prep = malloc(2 * nl * sizeof(ocrGuid_t));

  /* The rank's two numerical buffers, as the origin has them: the local rows
   * and the local transposed rows, one object each.  Every row task holds the
   * buffer its rows live in and addresses its own row by offset. */
  ocrGuid_t varr, warr;
  double *v0, *w0;
  ocrDbCreate(&varr, (void **)&v0, nxl * 2 * cy * sizeof(double), DB_PROP_NONE,
              &dh, NO_ALLOC);
  ocrDbCreate(&warr, (void **)&w0, nyl * 2 * cx * sizeof(double), DB_PROP_NONE,
              &dh, NO_ALLOC);
  /* Each row is zero over the two elements the real-to-complex layout adds
   * and the origin's ramp over the rest. */
  for (u64 i = 0; i < nxl; ++i) {
    double *d = v0 + i * 2 * cy;
    for (u64 c = 0; c < 2 * cy; ++c) d[c] = 0.0;
    for (u64 c = 0; c < ry; ++c) d[c] = (double)c;
  }
  memset(w0, 0, nyl * 2 * cx * sizeof(double));

  for (u64 j = 0; j < nl; ++j) {
    double *d;
    u64 size = nxl * pv[P_CYPART] * sizeof(double);
    ocrDbCreate(&prep[j], (void **)&d, size, DB_PROP_NONE, &dh, NO_ALLOC);
    memset(d, 0, size);
    ocrDbRelease(prep[j]);
    size = nyl * pv[P_CXPART] * sizeof(double);
    ocrDbCreate(&prep[nl + j], (void **)&d, size, DB_PROP_NONE, &dh, NO_ALLOC);
    memset(d, 0, size);
    ocrDbRelease(prep[nl + j]);
  }

  /* The two plans of the origin's initialize, built on the first row of each
   * buffer.  The buffers themselves are borrowed: the closure records their
   * identity and extent, never their bytes, so a row task lends the buffer it
   * acquired under the identity its plan was built on. */
  fftw_reloc_context *planner = fftw_reloc_context_create();
  fftw_reloc_bind_borrowed(planner, FFTW_RELOC_R2C_ARRAY, v0,
                             nxl * 2 * cy * sizeof(double),
                             mirror_guid_u64(varr));
  fftw_reloc_bind_borrowed(planner, FFTW_RELOC_C2C_ARRAY, w0,
                             nyl * 2 * cx * sizeof(double),
                             mirror_guid_u64(warr));
  unsigned flag = (unsigned)pv[P_PLANFLAG];
  u64 r2c = fftw_reloc_plan_r2c(planner, (int)ry, v0, flag);
  u64 c2c = fftw_reloc_plan_c2c(planner, (int)cx, w0, FFTW_FORWARD, flag);
  u64 plan_size = fftw_reloc_snapshot_size(planner);
  ocrGuid_t plan_db;
  void *plan_storage;
  ocrDbCreate(&plan_db, &plan_storage, fftw_reloc_image_reserve(plan_size),
              DB_PROP_NONE, &dh, NO_ALLOC);
  void *plan_image = fftw_reloc_image_at(plan_storage);
  fftw_reloc_snapshot_write(planner, plan_image);
  fftw_reloc_context_delete(planner);
  fftw_reloc_image_protect(plan_image, plan_size, 1);
  ocrDbRelease(plan_db);
  ocrDbRelease(varr);
  ocrDbRelease(warr);

  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_PLAN_R2C] = r2c;
  params[P_PLAN_C2C] = c2c;
  params[P_PLAN_DB] = mirror_guid_u64(plan_db);
  params[P_PLAN_SIZE] = plan_size;
  params[P_V_DB] = mirror_guid_u64(varr);
  params[P_W_DB] = mirror_guid_u64(warr);

  ocrGuid_t join_f1 = mirror_latch(nxl + 1), join_s1 = mirror_latch(nxl);
  ocrGuid_t join_f2 = mirror_latch(nyl), join_s2 = mirror_latch(nyl);
  ocrGuid_t xpose[2], xout[2];
  for (u64 phase = 0; phase < 2; ++phase) {
    params[P_PHASE] = phase;
    ocrEdtCreate(&xpose[phase], mirror_u64_guid(pv[P_XPOSE_SCOPE_TPL]),
                 P_COUNT, params, (u32)(1 + nl), NULL, EDT_PROP_FINISH, &eh,
                 &xout[phase]);
  }
  params[P_PHASE] = 0;

  ocrGuid_t e, out, reaped;
  ocrEdtCreate(&e, mirror_u64_guid(pv[P_REAP1_TPL]), P_COUNT, params,
               (u32)(1 + 2 * nl), NULL, EDT_PROP_NONE, &eh, &reaped);
  ocrAddDependence(join_s2, e, 0, DB_MODE_NULL);
  for (u64 q = 0; q < nl; ++q) {
    ocrGuid_t p = chunk_point(params, 0, q, rank);
    mirror_edge_open(p);
    ocrAddDependence(p, e, (u32)(1 + q), DB_MODE_RO);
    p = chunk_point(params, 1, q, rank);
    mirror_edge_open(p);
    ocrAddDependence(p, e, (u32)(1 + nl + q), DB_MODE_RO);
  }

  ocrEdtCreate(&e, mirror_u64_guid(pv[P_FINISH_TPL]), P_COUNT, params,
               (u32)(5 + nl), NULL, EDT_PROP_NONE, &eh, NULL);
  ocrAddDependence(xout[1], e, 0, DB_MODE_NULL);
  ocrAddDependence(reaped, e, 1, DB_MODE_NULL);
  for (u64 q = 0; q < nl; ++q) {
    ocrGuid_t p = chunk_point(params, 1, q, rank);
    mirror_edge_open(p);
    ocrAddDependence(p, e, (u32)(2 + q), DB_MODE_RO);
  }
  ocrAddDependence(varr, e, (u32)(2 + nl), DB_MODE_RO);
  ocrAddDependence(warr, e, (u32)(3 + nl), DB_MODE_RO);
  ocrAddDependence(plan_db, e, (u32)(4 + nl), DB_MODE_RW);

  for (u64 phase = 0; phase < 2; ++phase) {
    params[P_PHASE] = phase;
    ocrGuid_t join_split = phase ? join_s2 : join_s1;
    ocrEdtCreate(&e, mirror_u64_guid(pv[P_PUBLISH_TPL]), P_COUNT, params,
                 (u32)(1 + nl), NULL, EDT_PROP_NONE, &eh, NULL);
    ocrAddDependence(join_split, e, 0, DB_MODE_NULL);
    for (u64 j = 0; j < nl; ++j)
      ocrAddDependence(prep[phase * nl + j], e, (u32)(1 + j), DB_MODE_RO);

    ocrAddDependence(phase ? reaped : join_split, xpose[phase], 0, DB_MODE_NULL);
    for (u64 q = 0; q < nl; ++q) {
      ocrGuid_t p = chunk_point(params, phase, q, rank);
      mirror_edge_open(p);
      ocrAddDependence(p, xpose[phase], (u32)(1 + q), DB_MODE_RO);
    }

    u64 count = phase ? nyl : nxl;
    for (u64 i = 0; i < count; ++i) {
      params[P_IDX] = i;
      ocrEdtCreate(&e, mirror_u64_guid(pv[phase ? P_SPLIT2_TPL : P_SPLIT1_TPL]),
                   P_COUNT, params, (u32)(2 + nl), NULL, EDT_PROP_NONE, &eh, &out);
      ocrAddDependence(out, join_split, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
      ocrAddDependence(phase ? join_f2 : join_f1, e, 0, DB_MODE_NULL);
      ocrAddDependence(phase ? warr : varr, e, 1, DB_MODE_RO);
      for (u64 j = 0; j < nl; ++j)
        ocrAddDependence(prep[phase * nl + j], e, (u32)(2 + j), DB_MODE_RW);
    }
  }

  params[P_PHASE] = 0;
  for (u64 k = 0; k < nyl; ++k) {
    params[P_IDX] = k;
    ocrEdtCreate(&e, mirror_u64_guid(pv[P_FFT2_TPL]), P_COUNT, params, 3,
                 NULL, EDT_PROP_NONE, &eh, &out);
    ocrAddDependence(out, join_f2, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(xout[0], e, 0, DB_MODE_NULL);
    ocrAddDependence(plan_db, e, 2, DB_MODE_RO);
    ocrAddDependence(warr, e, 1, DB_MODE_RW);
  }

  for (u64 i = 0; i < nxl; ++i) {
    params[P_IDX] = i;
    ocrEdtCreate(&e, mirror_u64_guid(pv[P_FFT1_TPL]), P_COUNT, params, 2, NULL,
                 EDT_PROP_NONE, &eh, &out);
    ocrAddDependence(out, join_f1, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(plan_db, e, 1, DB_MODE_RO);
    ocrAddDependence(varr, e, 0, DB_MODE_RW);
  }
  ocrEventSatisfySlot(join_f1, NULL_GUID, OCR_EVENT_LATCH_DECR_SLOT);
  free(prep);
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 nx = 8, ny = 14;
  char plan[32] = "estimate", run[32] = "scatter";
  int bad = mirror_option_u64(argdb, argc, "--nx", &nx) < 0
         || mirror_option_u64(argdb, argc, "--ny", &ny) < 0
         || mirror_option_str(argdb, argc, "--plan", plan, sizeof plan) < 0
         || mirror_option_str(argdb, argc, "--run", run, sizeof run) < 0;
  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);

  /* The origin's own derivation, including its recomputation of the real
   * length from the complex one: an odd --ny transforms one point fewer. */
  u64 cx = nx, cy = ny / 2 + 1, ry = 2 * cy - 2;
  int sane = !bad && nl != 0 && nx != 0 && cy >= 2 && ry >= 2
             && cx % nl == 0 && cy % nl == 0
             && strcmp(run, "scatter") == 0;
  u64 nxl = sane ? cx / nl : 0, nyl = sane ? cy / nl : 0;
  u64 cypart = sane ? 2 * cy / nl : 0, cxpart = sane ? 2 * cx / nl : 0;
  /* The second transpose reads its chunk with the FIRST phase's input stride
   * -- the origin's own arithmetic, carried unchanged -- so its highest read
   * is cypart*(nyl-1) + 2*nxl - 1 against a chunk of nyl*cxpart doubles.
   * That is in bounds exactly when 2*(nyl-1)*(nyl-nxl) < 1, which over the
   * integers is nyl <= nxl or nyl == 1.  The row count the origin derives
   * from the arriving chunk, (nyl*cxpart)/cypart, is equal to nxl for every
   * input the divisibility test already admits, so it is an identity and
   * cannot stand in for this. */
  u64 points = 2;
  if (sane)
    sane = mirror_fits(&points, nl, 0xffffffffu)
           && mirror_fits(&points, nl, 0xffffffffu)
           && (nyl <= nxl || nyl == 1);

  ocrGuid_t range = NULL_GUID;
  if (sane && ocrGuidRangeCreate(&range, points, GUID_USER_EVENT_STICKY) != 0)
    sane = 0;
  if (!sane) {
    PRINTF("fft_hpx: usage --nx=X --ny=Y"
           " [--plan=estimate|measure|patient|exhaustive] [--run=scatter]"
           " (X and Y/2+1 each a multiple of the rank count, Y >= 2,"
           " Y/2+1 at most X or equal to the rank count,"
           " 2*ranks*ranks < 2^32; this row carries the scatter scheme,"
           " the origin's all_to_all is a different collective)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  /* The origin's plan-flag chain, else-less: an unrecognised name keeps the
   * estimate default rather than being rejected. */
  unsigned flag = FFTW_ESTIMATE;
  if (strcmp(plan, "measure") == 0) flag = FFTW_MEASURE;
  else if (strcmp(plan, "patient") == 0) flag = FFTW_PATIENT;
  else if (strcmp(plan, "exhaustive") == 0) flag = FFTW_EXHAUSTIVE;

  ocrGuid_t fft1_tpl, split1_tpl, xpose1_tpl, fft2_tpl, split2_tpl, xpose2_tpl;
  ocrGuid_t reap1_tpl, finish_tpl, sum_tpl, driver_tpl, publish_tpl;
  ocrGuid_t xpose_scope_tpl, xpose_outer_tpl;
  ocrEdtTemplateCreate(&fft1_tpl, fft1_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&split1_tpl, split1_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&xpose1_tpl, xpose1_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&fft2_tpl, fft2_edt, P_COUNT, 3);
  ocrEdtTemplateCreate(&split2_tpl, split2_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&xpose2_tpl, xpose2_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&reap1_tpl, reap1_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&finish_tpl, finish_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&sum_tpl, sum_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&driver_tpl, driver_edt, P_COUNT, 0);
  ocrEdtTemplateCreate(&publish_tpl, publish_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&xpose_scope_tpl, xpose_scope_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&xpose_outer_tpl, xpose_outer_edt, P_COUNT, 1);

  u64 pv[P_COUNT] = {0};
  pv[P_NL] = nl; pv[P_NXL] = nxl; pv[P_NYL] = nyl;
  pv[P_CX] = cx; pv[P_CY] = cy; pv[P_RY] = ry;
  pv[P_CYPART] = cypart; pv[P_CXPART] = cxpart; pv[P_PLANFLAG] = flag;
  pv[P_RANGE] = mirror_guid_u64(range);
  pv[P_FFT1_TPL] = mirror_guid_u64(fft1_tpl);
  pv[P_SPLIT1_TPL] = mirror_guid_u64(split1_tpl);
  pv[P_XPOSE1_TPL] = mirror_guid_u64(xpose1_tpl);
  pv[P_FFT2_TPL] = mirror_guid_u64(fft2_tpl);
  pv[P_SPLIT2_TPL] = mirror_guid_u64(split2_tpl);
  pv[P_XPOSE2_TPL] = mirror_guid_u64(xpose2_tpl);
  pv[P_REAP1_TPL] = mirror_guid_u64(reap1_tpl);
  pv[P_FINISH_TPL] = mirror_guid_u64(finish_tpl);
  pv[P_PUBLISH_TPL] = mirror_guid_u64(publish_tpl);
  pv[P_XPOSE_SCOPE_TPL] = mirror_guid_u64(xpose_scope_tpl);
  pv[P_XPOSE_OUTER_TPL] = mirror_guid_u64(xpose_outer_tpl);

  ocrHint_t h0;
  mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t sum;
  ocrEdtCreate(&sum, sum_tpl, P_COUNT, pv, (u32)nl, NULL, EDT_PROP_NONE, &h0,
               NULL);
  pv[P_SUM_EDT] = mirror_guid_u64(sum);

  mirror_spmd_fork(driver_tpl, pv, P_COUNT, P_RANK, nl, 0, NULL);
  return NULL_GUID;
}
