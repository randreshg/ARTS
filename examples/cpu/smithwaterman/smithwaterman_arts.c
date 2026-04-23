/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/

/*
 * Smith-Waterman (local alignment) — native ARTS port.
 *
 * Ports third_party/ocr-apps/apps/smithwaterman/ocr/smithwaterman.c to native
 * ARTS in LULESH style, with ocrPNNL's distributed tile-owner strategy
 * (round-robin chunk=8) reimplemented via arts_hint_t.route.
 *
 * Algorithm:
 *   Standard wavefront DP.  Strings are laid out as a 2-D tile grid of
 *   (n_tiles_h x n_tiles_w) tiles of size (tile_h x tile_w) cells.  Tile
 *   (i,j) needs 3 halos from neighbors: right-column of (i,j-1), bottom-
 *   row of (i-1,j), and bottom-right of (i-1,j-1).  Each tile emits its
 *   own 3 halos for downstream tiles.  Boundary tiles receive GAP-penalty
 *   halos from main_edt.
 *
 * DB granularity (per tile):
 *   3 output halo DBs — right_col (tile_h * int32), bottom_row (tile_w *
 *   int32), bottom_right (1 * int32).  Each halo DB is created once,
 *   consumed once, destroyed.  No concurrent access, no CDAG frontier
 *   pressure.
 *
 * Synchronization:
 *   fibDB pattern: arts_edt_create allocates depc slots; arts_signal_edt
 *   pushes DB payloads directly to target EDT slots.  No events needed.
 *   Pattern A release-before-signal is used on every halo DB for CXL
 *   retrofit.
 *
 * Distribution:
 *   tile_owner(i,j) = (tile_linear_index(i,j) / CHUNK_SIZE) % num_nodes,
 *   CHUNK_SIZE = 8 (ocrPNNL default).
 *
 * Shared data:
 *   One params DB holds tile/grid dims and both strings.  Every tile EDT
 *   has an RO dep on it at slot 3.  Read-only, never modified.
 *
 * paramv layout for sw_tile_edt (9 words):
 *   [0] i
 *   [1] j
 *   [2] rc_edt_guid      — downstream tile (i, j+1) EDT GUID
 *   [3] rc_slot          — slot in rc_edt that receives right_col halo
 *   [4] brow_edt_guid    — downstream tile (i+1, j) EDT GUID
 *   [5] brow_slot        — slot in brow_edt that receives bottom_row halo
 *   [6] bcorner_edt_guid — downstream tile (i+1, j+1) EDT GUID
 *   [7] bcorner_slot     — slot in bcorner_edt that receives corner halo
 *   [8] done_edt_guid    — done EDT GUID (NULL_GUID for non-last tiles)
 *
 * depv layout for sw_tile_edt (4 slots):
 *   [0] left halo  (right_col of (i, j-1))
 *   [1] top  halo  (bottom_row of (i-1, j))
 *   [2] NW   halo  (bottom_right of (i-1, j-1))
 *   [3] params DB  (RO shared)
 */

#include "arts.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAP_PENALTY (-1)
#define TRANSITION_PENALTY (-2)
#define TRANSVERSION_PENALTY (-4)
#define MATCH (2)
#define CHUNK_SIZE (8)

enum Nucleotide { GAP = 0, ADENINE, CYTOSINE, GUANINE, THYMINE };

static const int alignment_score_matrix[5][5] = {
    {GAP_PENALTY, GAP_PENALTY, GAP_PENALTY, GAP_PENALTY, GAP_PENALTY},
    {GAP_PENALTY, MATCH, TRANSVERSION_PENALTY, TRANSITION_PENALTY,
     TRANSVERSION_PENALTY},
    {GAP_PENALTY, TRANSVERSION_PENALTY, MATCH, TRANSVERSION_PENALTY,
     TRANSITION_PENALTY},
    {GAP_PENALTY, TRANSITION_PENALTY, TRANSVERSION_PENALTY, MATCH,
     TRANSVERSION_PENALTY},
    {GAP_PENALTY, TRANSVERSION_PENALTY, TRANSITION_PENALTY,
     TRANSVERSION_PENALTY, MATCH}};

static inline int char_map(char c) {
  switch (c) {
  case 'A':
    return ADENINE;
  case 'C':
    return CYTOSINE;
  case 'G':
    return GUANINE;
  case 'T':
    return THYMINE;
  default:
    return -1;
  }
}

/* ========================================================================= */
/*  Global state (built on every node by init_per_node)                      */
/* ========================================================================= */

typedef struct {
  int n_tiles_h;    /* number of row tiles */
  int n_tiles_w;    /* number of col tiles */
  int tile_h;       /* cells per tile (row direction) */
  int tile_w;       /* cells per tile (col direction) */
  int string1_len;  /* length of string 1 (laid along width) */
  int string2_len;  /* length of string 2 (laid along height) */
  int verify_score; /* expected final DP score (from file) */
} sw_topo_t;

static sw_topo_t g_topo;

static inline unsigned tile_owner(int i, int j) {
  int linear = (i - 1) * g_topo.n_tiles_w + (j - 1);
  return (unsigned)((linear / CHUNK_SIZE) % arts_get_total_nodes());
}

/* ========================================================================= */
/*  Shared params DB layout                                                   */
/*                                                                           */
/*  [int tile_w, tile_h, n_tiles_h, n_tiles_w, string1_len, string2_len,    */
/*   string1_off, string2_off, verify_score, reserved[7],                   */
/*   string1[string1_len], string2[string2_len]]                             */
/* ========================================================================= */

#define PARAMS_HDR_WORDS 16

static inline int params_tile_w(const int64_t *p) { return (int)p[0]; }
static inline int params_tile_h(const int64_t *p) { return (int)p[1]; }
static inline int params_n_h(const int64_t *p) { return (int)p[2]; }
static inline int params_n_w(const int64_t *p) { return (int)p[3]; }
static inline int params_s1_len(const int64_t *p) { return (int)p[4]; }
static inline int params_s2_len(const int64_t *p) { return (int)p[5]; }
static inline const int8_t *params_s1(const int64_t *p) {
  return (const int8_t *)&p[PARAMS_HDR_WORDS];
}
static inline const int8_t *params_s2(const int64_t *p) {
  int s1_bytes = params_s1_len(p);
  int s1_words = (s1_bytes + 7) / 8;
  return (const int8_t *)&p[PARAMS_HDR_WORDS + s1_words];
}
static inline int params_score(const int64_t *p) { return (int)p[8]; }

/* ========================================================================= */
/*  Forward decls                                                            */
/* ========================================================================= */

void sw_tile_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]);

void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]);

/* ========================================================================= */
/*  done_edt  (always runs on node 0)                                        */
/*                                                                           */
/*  paramv[0] = expected score (verify_score from params)                    */
/*  depv[0]   = score DB (int32_t[1]) RO — final DP score from last tile     */
/* ========================================================================= */

void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int expected = (int)paramv[0];
  const int32_t *score_db = (const int32_t *)depv[0].ptr;
  int final_score = score_db[0];
  arts_printf("SW-ARTS final score = %d (expected %d) %s\n", final_score,
              expected, (final_score == expected) ? "PASS" : "FAIL");
  arts_shutdown();
}

/* ========================================================================= */
/*  sw_tile_edt                                                              */
/*                                                                           */
/*  paramv[0] = i, paramv[1] = j                                             */
/*  paramv[2] = rc_edt_guid      (NULL_GUID if no right neighbor)            */
/*  paramv[3] = rc_slot          (slot in rc_edt for right_col halo)         */
/*  paramv[4] = brow_edt_guid    (NULL_GUID if no bottom neighbor)           */
/*  paramv[5] = brow_slot        (slot in brow_edt for bottom_row halo)      */
/*  paramv[6] = bcorner_edt_guid (NULL_GUID if no bottom-right neighbor)     */
/*  paramv[7] = bcorner_slot     (slot in bcorner_edt for corner halo)       */
/*  paramv[8] = done_edt_guid    (NULL_GUID for non-last tiles)              */
/*  depv[0] = left halo (right_col of (i, j-1)) RO                           */
/*  depv[1] = top  halo (bottom_row of (i-1, j)) RO                          */
/*  depv[2] = NW   halo (bottom_right of (i-1, j-1)) RO                      */
/*  depv[3] = params DB RO                                                   */
/* ========================================================================= */

void sw_tile_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int i = (int)paramv[0];
  int j = (int)paramv[1];
  arts_guid_t rc_edt_guid      = (arts_guid_t)paramv[2];
  uint32_t    rc_slot          = (uint32_t)paramv[3];
  arts_guid_t brow_edt_guid    = (arts_guid_t)paramv[4];
  uint32_t    brow_slot        = (uint32_t)paramv[5];
  arts_guid_t bcorner_edt_guid = (arts_guid_t)paramv[6];
  uint32_t    bcorner_slot     = (uint32_t)paramv[7];
  arts_guid_t done_guid        = (arts_guid_t)paramv[8];

  const int32_t *left_col = (const int32_t *)depv[0].ptr;
  const int32_t *top_row  = (const int32_t *)depv[1].ptr;
  const int32_t *nw_br    = (const int32_t *)depv[2].ptr;
  const int64_t *params   = (const int64_t *)depv[3].ptr;

  int tile_w     = params_tile_w(params);
  int tile_h     = params_tile_h(params);
  int n_tiles_w  = params_n_w(params);
  int n_tiles_h  = params_n_h(params);
  int string1_len = params_s1_len(params);
  int string2_len = params_s2_len(params);
  const int8_t *string_1 = params_s1(params);
  const int8_t *string_2 = params_s2(params);

  int eff_w = tile_w;
  int eff_h = tile_h;
  if (j == n_tiles_w) {
    int rem = string1_len - (j - 1) * tile_w;
    if (rem < tile_w && rem > 0)
      eff_w = rem;
  }
  if (i == n_tiles_h) {
    int rem = string2_len - (i - 1) * tile_h;
    if (rem < tile_h && rem > 0)
      eff_h = rem;
  }

  /* Local DP scratchpad: (tile_h+1) x (tile_w+1) ints */
  size_t cells = (size_t)(tile_h + 1) * (size_t)(tile_w + 1);
  int32_t *M = (int32_t *)malloc(cells * sizeof(int32_t));
  int W = tile_w + 1;
#define Mref(r, c) M[(size_t)(r) * (size_t)W + (size_t)(c)]

  /* Seed halo */
  Mref(0, 0) = nw_br[0];
  for (int r = 1; r <= eff_h; ++r)
    Mref(r, 0) = left_col[r - 1];
  for (int c = 1; c <= eff_w; ++c)
    Mref(0, c) = top_row[c - 1];

  /* DP */
  for (int ii = 1; ii <= eff_h; ++ii) {
    for (int jj = 1; jj <= eff_w; ++jj) {
      int c1 = string_1[(j - 1) * tile_w + (jj - 1)];
      int c2 = string_2[(i - 1) * tile_h + (ii - 1)];
      int diag = Mref(ii - 1, jj - 1) + alignment_score_matrix[c2][c1];
      int left = Mref(ii, jj - 1) + alignment_score_matrix[c1][GAP];
      int top  = Mref(ii - 1, jj) + alignment_score_matrix[GAP][c2];
      int bigger = (left > top) ? left : top;
      Mref(ii, jj) = (bigger > diag) ? bigger : diag;
    }
  }

  unsigned cur = arts_get_current_node();

  /* Bottom-right corner halo → downstream tile (i+1, j+1) slot bcorner_slot */
  if (bcorner_edt_guid != NULL_GUID) {
    int32_t *br_corner_data;
#if ARTS_USE_CXL
    arts_guid_t br_corner_guid = arts_db_create((void **)&br_corner_data,
                                                sizeof(int32_t), ARTS_DB_CXL, NULL);
#else
    arts_guid_t br_corner_guid =
        arts_db_create((void **)&br_corner_data, sizeof(int32_t),
                       ARTS_DB_DEFAULT, &(arts_hint_t){.route = cur});
#endif
    br_corner_data[0] = Mref(eff_h, eff_w);
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(br_corner_guid);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(br_corner_guid); /* Pattern A */
    arts_signal_edt(bcorner_edt_guid, bcorner_slot, br_corner_guid, DB_MODE_RO);
  }

  /* Right column halo → downstream tile (i, j+1) slot rc_slot */
  if (rc_edt_guid != NULL_GUID) {
    int32_t *rc_data;
#if ARTS_USE_CXL
    arts_guid_t rc_guid = arts_db_create((void **)&rc_data,
                                         sizeof(int32_t) * tile_h, ARTS_DB_CXL, NULL);
#else
    arts_guid_t rc_guid =
        arts_db_create((void **)&rc_data, sizeof(int32_t) * tile_h,
                       ARTS_DB_DEFAULT, &(arts_hint_t){.route = cur});
#endif
    for (int r = 0; r < eff_h; ++r)
      rc_data[r] = Mref(r + 1, eff_w);
    for (int r = eff_h; r < tile_h; ++r)
      rc_data[r] = 0; /* pad */
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(rc_guid);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(rc_guid); /* Pattern A */
    arts_signal_edt(rc_edt_guid, rc_slot, rc_guid, DB_MODE_RO);
  }

  /* Bottom row halo → downstream tile (i+1, j) slot brow_slot */
  if (brow_edt_guid != NULL_GUID) {
    int32_t *br_row_data;
#if ARTS_USE_CXL
    arts_guid_t br_row_guid = arts_db_create((void **)&br_row_data,
                                             sizeof(int32_t) * tile_w, ARTS_DB_CXL, NULL);
#else
    arts_guid_t br_row_guid =
        arts_db_create((void **)&br_row_data, sizeof(int32_t) * tile_w,
                       ARTS_DB_DEFAULT, &(arts_hint_t){.route = cur});
#endif
    for (int c = 0; c < eff_w; ++c)
      br_row_data[c] = Mref(eff_h, c + 1);
    for (int c = eff_w; c < tile_w; ++c)
      br_row_data[c] = 0; /* pad */
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(br_row_guid);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(br_row_guid); /* Pattern A */
    arts_signal_edt(brow_edt_guid, brow_slot, br_row_guid, DB_MODE_RO);
  }

  int final_score = Mref(eff_h, eff_w);
  free(M);

  /* If this is the last tile (bottom-right most), signal done_edt on node 0
   * which will print the result and shut down. */
  if (i == n_tiles_h && j == n_tiles_w && done_guid != NULL_GUID) {
    int32_t *score_data;
#if ARTS_USE_CXL
    arts_guid_t score_db_guid = arts_db_create(
        (void **)&score_data, sizeof(int32_t), ARTS_DB_CXL, NULL);
#else
    arts_guid_t score_db_guid =
        arts_db_create((void **)&score_data, sizeof(int32_t), ARTS_DB_DEFAULT,
                       &(arts_hint_t){.route = 0});
#endif
    score_data[0] = final_score;
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(score_db_guid);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(score_db_guid); /* Pattern A */
    arts_signal_edt(done_guid, 0, score_db_guid, DB_MODE_RO);
  }
#undef Mref
}

/* ========================================================================= */
/*  File I/O (reads on rank 0 only, from main_edt)                           */
/* ========================================================================= */

static char *read_file_whitespace_mapped(const char *path, int *out_len) {
  FILE *f = fopen(path, "r");
  if (!f) {
    arts_printf("SW: cannot open %s\n", path);
    return NULL;
  }
  fseek(f, 0L, SEEK_END);
  long fsz = ftell(f);
  fseek(f, 0L, SEEK_SET);
  char *buf = (char *)malloc((size_t)fsz + 1);
  size_t got = fread(buf, 1, (size_t)fsz, f);
  (void)got;
  buf[fsz] = '\0';
  fclose(f);

  /* Strip whitespace, map to enum */
  int n = 0;
  for (long k = 0; k < fsz; ++k) {
    char c = buf[k];
    if (c == 'A' || c == 'C' || c == 'G' || c == 'T') {
      buf[n++] = (char)char_map(c);
    }
  }
  *out_len = n;
  return buf;
}

/* ========================================================================= */
/*  main_edt                                                                 */
/* ========================================================================= */

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  if (argc < 6) {
    arts_printf("Usage: %s <tile_w> <tile_h> <string1_file> <string2_file> "
                "<score_file>\n",
                argv[0]);
    arts_shutdown();
    return;
  }
  int tile_w = atoi(argv[1]);
  int tile_h = atoi(argv[2]);
  int s1_len = 0;
  int s2_len = 0;
  char *s1 = read_file_whitespace_mapped(argv[3], &s1_len);
  char *s2 = read_file_whitespace_mapped(argv[4], &s2_len);
  if (!s1 || !s2) {
    arts_shutdown();
    return;
  }
  int score_len = 0;
  char *score_buf = read_file_whitespace_mapped(argv[5], &score_len);
  (void)score_buf;
  /* read_file_whitespace_mapped strips chars — for score file read raw */
  int verify_score = 0;
  {
    FILE *f = fopen(argv[5], "r");
    if (f) {
      if (fscanf(f, "%d", &verify_score) != 1)
        verify_score = 0;
      fclose(f);
    }
  }
  int n_tiles_w = (s1_len + tile_w - 1) / tile_w;
  int n_tiles_h = (s2_len + tile_h - 1) / tile_h;

  g_topo.tile_w      = tile_w;
  g_topo.tile_h      = tile_h;
  g_topo.n_tiles_w   = n_tiles_w;
  g_topo.n_tiles_h   = n_tiles_h;
  g_topo.string1_len = s1_len;
  g_topo.string2_len = s2_len;
  g_topo.verify_score = verify_score;

  unsigned nn = arts_get_total_nodes();
  arts_printf("SW-ARTS: tile_w=%d tile_h=%d string1=%d string2=%d "
              "n_tiles=%dx%d nodes=%u score_expect=%d\n",
              tile_w, tile_h, s1_len, s2_len, n_tiles_h, n_tiles_w, nn,
              verify_score);

  /* --- Build params DB (shared RO across all tile EDTs) --- */
  int s1_words = (s1_len + 7) / 8;
  int s2_words = (s2_len + 7) / 8;
  size_t params_bytes = ((size_t)PARAMS_HDR_WORDS + s1_words + s2_words) * 8;
  int64_t *params;
#if ARTS_USE_CXL
  arts_guid_t params_guid =
      arts_db_create((void **)&params, params_bytes, ARTS_DB_CXL, NULL);
#else
  arts_guid_t params_guid =
      arts_db_create((void **)&params, params_bytes, ARTS_DB_DEFAULT,
                     &(arts_hint_t){.route = 0});
#endif
  memset(params, 0, params_bytes);
  params[0] = tile_w;
  params[1] = tile_h;
  params[2] = n_tiles_h;
  params[3] = n_tiles_w;
  params[4] = s1_len;
  params[5] = s2_len;
  params[8] = verify_score;
  memcpy((char *)&params[PARAMS_HDR_WORDS], s1, (size_t)s1_len);
  memcpy((char *)&params[PARAMS_HDR_WORDS + s1_words], s2, (size_t)s2_len);
  free(s1);
  free(s2);
  if (score_buf)
    free(score_buf);
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
  arts_cxl_producer_flush(params_guid);
#endif
#endif /* ARTS_USE_CXL */
  arts_db_release(params_guid); /* WRITE — shared params ready */

  /* --- Create done_edt on node 0 (1 dep slot: score DB) --- */
  uint64_t dp[1] = {(uint64_t)verify_score};
  arts_guid_t done_edt_guid =
      arts_edt_create(done_edt, 1, dp, 1, &(arts_hint_t){.route = 0});

  /* --- Allocate tile EDT GUID table: (n_tiles_h+1) x (n_tiles_w+1)
   *     Row/col 0 are unused (boundary); real tiles at [1..n_tiles_h][1..n_tiles_w].
   *     NULL_GUID marks "no downstream EDT" for border tiles. --- */
  int rows = n_tiles_h + 1;
  int cols = n_tiles_w + 1;
  arts_guid_t *tile_guids =
      (arts_guid_t *)calloc((size_t)rows * cols, sizeof(arts_guid_t));
#define TGUID(r, c) tile_guids[(size_t)(r) * (size_t)cols + (size_t)(c)]

  /* First pass: reserve GUIDs for all tile EDTs so downstream GUIDs are
   * known before we build each tile's paramv. */
  for (int i = 1; i <= n_tiles_h; ++i) {
    for (int j = 1; j <= n_tiles_w; ++j) {
      unsigned o = tile_owner(i, j);
      TGUID(i, j) = arts_guid_reserve(ARTS_EDT, o);
    }
  }

  /* Second pass: create each EDT with the correct paramv that references
   * downstream GUIDs.  arts_edt_create_with_guid uses the pre-reserved GUID
   * (route is already encoded in the GUID; no hint parameter). */
  for (int i = 1; i <= n_tiles_h; ++i) {
    for (int j = 1; j <= n_tiles_w; ++j) {
      /* Downstream right neighbor (i, j+1) — receives right_col at slot 0 */
      arts_guid_t rc_edt  = (j < n_tiles_w) ? TGUID(i, j + 1) : NULL_GUID;
      uint32_t    rc_slot = 0; /* left halo slot */

      /* Downstream bottom neighbor (i+1, j) — receives bottom_row at slot 1 */
      arts_guid_t brow_edt  = (i < n_tiles_h) ? TGUID(i + 1, j) : NULL_GUID;
      uint32_t    brow_slot = 1; /* top halo slot */

      /* Downstream bottom-right neighbor (i+1, j+1) — receives corner at slot 2 */
      arts_guid_t bcorner_edt  = (i < n_tiles_h && j < n_tiles_w)
                                     ? TGUID(i + 1, j + 1)
                                     : NULL_GUID;
      uint32_t    bcorner_slot = 2; /* NW halo slot */

      int is_last = (i == n_tiles_h && j == n_tiles_w);

      uint64_t p[9] = {
          (uint64_t)i,
          (uint64_t)j,
          (uint64_t)rc_edt,
          (uint64_t)rc_slot,
          (uint64_t)brow_edt,
          (uint64_t)brow_slot,
          (uint64_t)bcorner_edt,
          (uint64_t)bcorner_slot,
          (uint64_t)(is_last ? done_edt_guid : NULL_GUID)};

      arts_edt_create_with_guid(sw_tile_edt, TGUID(i, j), 9, p, 4);
    }
  }

  /* --- Push params DB to slot 3 of every tile EDT --- */
  for (int i = 1; i <= n_tiles_h; ++i) {
    for (int j = 1; j <= n_tiles_w; ++j) {
      arts_signal_edt(TGUID(i, j), 3, params_guid, DB_MODE_RO);
    }
  }

  /* --- Initialize boundary halos via arts_signal_edt --- */

  /* (0,0) bottom-right corner → tile (1,1) slot 2 (NW halo) */
  {
    int32_t *p0;
#if ARTS_USE_CXL
    arts_guid_t g0 =
        arts_db_create((void **)&p0, sizeof(int32_t), ARTS_DB_CXL, NULL);
#else
    arts_guid_t g0 =
        arts_db_create((void **)&p0, sizeof(int32_t), ARTS_DB_DEFAULT,
                       &(arts_hint_t){.route = 0});
#endif
    p0[0] = 0;
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(g0);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(g0); /* Pattern A */
    arts_signal_edt(TGUID(1, 1), 2, g0, DB_MODE_RO);
  }

  /* Top row of halos: for each tile (1, j):
   *   slot 1 (top halo)  ← bottom_row of boundary row (0, j)
   *   slot 2 (NW halo)   ← bottom_right of boundary (0, j-1)  [already done for j=1 above]
   */
  for (int j = 1; j <= n_tiles_w; ++j) {
    int eff_w = tile_w;
    if (j == n_tiles_w) {
      int rem = s1_len - (j - 1) * tile_w;
      if (rem < tile_w && rem > 0)
        eff_w = rem;
    }

    /* bottom_row for boundary (0,j): GAP-penalty values along string 1
     * → pushed to tile (1, j) slot 1 (top halo) */
    int32_t *brow;
#if ARTS_USE_CXL
    arts_guid_t g_brow = arts_db_create(
        (void **)&brow, sizeof(int32_t) * tile_w, ARTS_DB_CXL, NULL);
#else
    arts_guid_t g_brow =
        arts_db_create((void **)&brow, sizeof(int32_t) * tile_w,
                       ARTS_DB_DEFAULT, &(arts_hint_t){.route = 0});
#endif
    for (int c = 0; c < eff_w; ++c)
      brow[c] = GAP_PENALTY * ((j - 1) * tile_w + c + 1);
    for (int c = eff_w; c < tile_w; ++c)
      brow[c] = 0;
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(g_brow);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(g_brow); /* Pattern A */
    arts_signal_edt(TGUID(1, j), 1, g_brow, DB_MODE_RO);

    /* bottom_right for boundary (0, j): scalar GAP*(cumulative width)
     * → pushed to tile (1, j+1) slot 2 (NW halo), if j+1 exists */
    if (j < n_tiles_w) {
      int32_t *bcor;
#if ARTS_USE_CXL
      arts_guid_t g_bcor =
          arts_db_create((void **)&bcor, sizeof(int32_t), ARTS_DB_CXL, NULL);
#else
      arts_guid_t g_bcor =
          arts_db_create((void **)&bcor, sizeof(int32_t), ARTS_DB_DEFAULT,
                         &(arts_hint_t){.route = 0});
#endif
      bcor[0] = GAP_PENALTY * ((j - 1) * tile_w + eff_w);
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
      arts_cxl_producer_flush(g_bcor);
#endif
#endif /* ARTS_USE_CXL */
      arts_db_release(g_bcor); /* Pattern A */
      arts_signal_edt(TGUID(1, j + 1), 2, g_bcor, DB_MODE_RO);
    }
  }

  /* Left column of halos: for each tile (i, 1):
   *   slot 0 (left halo) ← right_col of boundary col (i, 0)
   *   slot 2 (NW halo)   ← bottom_right of boundary (i-1, 0)  [already done for i=1 above]
   */
  for (int i = 1; i <= n_tiles_h; ++i) {
    int eff_h = tile_h;
    if (i == n_tiles_h) {
      int rem = s2_len - (i - 1) * tile_h;
      if (rem < tile_h && rem > 0)
        eff_h = rem;
    }

    /* right_col for boundary (i, 0): GAP-penalty values along string 2
     * → pushed to tile (i, 1) slot 0 (left halo) */
    int32_t *rc;
#if ARTS_USE_CXL
    arts_guid_t g_rc = arts_db_create((void **)&rc, sizeof(int32_t) * tile_h,
                                      ARTS_DB_CXL, NULL);
#else
    arts_guid_t g_rc =
        arts_db_create((void **)&rc, sizeof(int32_t) * tile_h, ARTS_DB_DEFAULT,
                       &(arts_hint_t){.route = 0});
#endif
    for (int r = 0; r < eff_h; ++r)
      rc[r] = GAP_PENALTY * ((i - 1) * tile_h + r + 1);
    for (int r = eff_h; r < tile_h; ++r)
      rc[r] = 0;
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
    arts_cxl_producer_flush(g_rc);
#endif
#endif /* ARTS_USE_CXL */
    arts_db_release(g_rc); /* Pattern A */
    arts_signal_edt(TGUID(i, 1), 0, g_rc, DB_MODE_RO);

    /* bottom_right for boundary (i, 0): scalar GAP*(cumulative height)
     * → pushed to tile (i+1, 1) slot 2 (NW halo), if i+1 exists */
    if (i < n_tiles_h) {
      int32_t *bcor;
#if ARTS_USE_CXL
      arts_guid_t g_bcor =
          arts_db_create((void **)&bcor, sizeof(int32_t), ARTS_DB_CXL, NULL);
#else
      arts_guid_t g_bcor =
          arts_db_create((void **)&bcor, sizeof(int32_t), ARTS_DB_DEFAULT,
                         &(arts_hint_t){.route = 0});
#endif
      bcor[0] = GAP_PENALTY * ((i - 1) * tile_h + eff_h);
#if ARTS_USE_CXL
#if !ARTS_CXL_ENABLE_AUTO_FLUSH
      arts_cxl_producer_flush(g_bcor);
#endif
#endif /* ARTS_USE_CXL */
      arts_db_release(g_bcor); /* Pattern A */
      arts_signal_edt(TGUID(i + 1, 1), 2, g_bcor, DB_MODE_RO);
    }
  }

  free(tile_guids);
#undef TGUID
}

void init_per_node(unsigned int node_id, int argc, char **argv) {
  (void)node_id;
  /* Parse same args so g_topo is populated on each node for sw_tile_edt. */
  if (argc < 3)
    return;
  g_topo.tile_w = atoi(argv[1]);
  g_topo.tile_h = atoi(argv[2]);
  /* Other fields are carried via paramv/depv deps (params DB); g_topo
   * isn't strictly needed on non-rank-0 nodes for sw_tile_edt. */
}

int main(int argc, char *argv[]) {
  arts_rt(argc, argv);
  return 0;
}
