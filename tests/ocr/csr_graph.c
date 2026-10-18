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
/// @file csr_graph.c
/// @brief The graph library under its contract: every distribution built in
///        init_per_node, partitions loaded by an EDT on each rank, read
///        through RO dependences once every loader has finished, destroyed
///        once every reader has.
///
/// Each partition is read by one EDT on every rank, so on n ranks n - 1 of
/// every n reads are of a block homed elsewhere, created under a GUID reserved
/// before parallel start.  Each reader checks its own oracle and reports a
/// failure through arts_test_fail(), so one run states every property below:
///   * a CSR built from edges is well formed: every local vertex's neighbour
///     range starts where the previous one ends, vertices with no out-edges
///     included, each range holds exactly that vertex's edges, the local edge
///     count is their sum, and a vertex's local index maps back to it through
///     its partition's start; a partition with no edges sorts and builds an
///     empty CSR without touching the (absent) edge array; a build for a
///     partition homed on another rank returns NULL;
///   * both loaders sort each row's targets in ascending order;
///   * the edge-list loader reads a file that opens with '%' lines as 1-based,
///     skips one size line after them only when the first line is the
///     "%%MatrixMarket" banner (a KONECT-style header has none), takes a '%'
///     or '#' line after the first data line as a comment and nothing more,
///     skips blank lines, takes a line's first two fields as its edge (a
///     value column is no endpoint), drops self loops when asked, adds every
///     reverse edge when flipping, and refuses a line with one field or an
///     endpoint at or above the vertex count (after the 1-based shift, self
///     loops included);
///   * the 1-based adjacency loader decrements targets, separates them by any
///     whitespace, the vertical tab included, skips a 0 token, which has no
///     0-based vertex, adds every reverse edge when flipping, and refuses a
///     header without an edge count, a header that disagrees with the line
///     count, and an endpoint at or above the vertex count;
///   * a refused load returns -1 and creates no partition;
///   * argument dispatch selects the loader and its options, refuses a missing
///     path, and never reads a flag's value past the end of the argument
///     array, for the loaders and for the distribution constructor alike.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arts.h"
#include "arts/graph.h"

#include "../test_failure_status.h"

typedef struct {
  arts_vertex_t s;
  arts_vertex_t t;
} edge_t;

enum build { EDGE_LIST, ADJACENCY, ARGS, DIRECT, DIRECT_EMPTY };

typedef struct {
  const char *name;
  enum build build;
  /* The fixture each loader writes, or NULL for none; "@" in args names it. */
  const char *file;
  arts_graph_sz_t vertices;
  bool flip;
  bool ignore_self_loops;
  const char *args[3];
  int nargs;
  int rc;
  const edge_t *edges;
  unsigned int nedges;
} csr_case_t;

#define EDGES(...)                                                             \
  .edges = (const edge_t[]){__VA_ARGS__},                                      \
  .nedges = sizeof((const edge_t[]){__VA_ARGS__}) / sizeof(edge_t)

static const csr_case_t cases[] = {
    /* Eight vertices; 2, 5 and 6 have no out-edges (interior gaps, one just
     * before the last source).  Its distribution is the constructor's from
     * arguments, one partition per rank. */
    {.name = "direct",
     .build = DIRECT,
     .vertices = 8,
     EDGES({0, 3}, {1, 0}, {1, 4}, {3, 2}, {3, 5}, {3, 7}, {4, 1}, {7, 0},
           {7, 6})},
    {.name = "direct empty", .build = DIRECT_EMPTY, .vertices = 8},
    /* MatrixMarket coordinate data: a '%' banner, its size line, a '#'
     * comment, then 1-based entries: after the shift the self loop 3->3 and
     * the entry 4->5 with a value column. */
    {.name = "edge list",
     .build = EDGE_LIST,
     .file = "%%MatrixMarket matrix coordinate real general\n"
             "6 6 6\n"
             "# a comment line\n"
             "1 2\n"
             "1 3\n"
             "2 1\n"
             "4 4\n"
             "5 6 99\n"
             "6 1\n",
     .vertices = 6,
     .ignore_self_loops = true,
     EDGES({0, 1}, {0, 2}, {1, 0}, {4, 5}, {5, 0})},
    {.name = "edge list flipped",
     .build = EDGE_LIST,
     .file = "%%MatrixMarket matrix coordinate real general\n"
             "6 6 6\n"
             "# a comment line\n"
             "1 2\n"
             "1 3\n"
             "2 1\n"
             "4 4\n"
             "5 6 99\n"
             "6 1\n",
     .vertices = 6,
     .flip = true,
     .ignore_self_loops = true,
     EDGES({0, 1}, {0, 2}, {1, 0}, {4, 5}, {5, 0}, {1, 0}, {2, 0}, {0, 1},
           {5, 4}, {0, 5})},
    /* Self loops kept: the blank line is no edge, so no 0->0 appears. */
    {.name = "edge list blank line",
     .build = EDGE_LIST,
     .file = "0 1\n\n2 3\n",
     .vertices = 6,
     EDGES({0, 1}, {2, 3})},
    /* A '%' line after the first data line is a comment and nothing more: the
     * ids stay 0-based and the line after it is an edge. */
    {.name = "edge list mid-file comment",
     .build = EDGE_LIST,
     .file = "0 1\n% a note\n2 3\n4 5\n",
     .vertices = 6,
     EDGES({0, 1}, {2, 3}, {4, 5})},
    /* A KONECT-style header: '%' lines and no size line, 1-based ids, the
     * largest naming the last vertex. */
    {.name = "edge list KONECT",
     .build = EDGE_LIST,
     .file = "% sym unweighted\n% 3 3 3\n1 2\n2 3\n3 1\n",
     .vertices = 3,
     EDGES({0, 1}, {1, 2}, {2, 0})},
    {.name = "edge list row order",
     .build = EDGE_LIST,
     .file = "2 3\n0 5\n0 1\n2 0\n0 3\n",
     .vertices = 6,
     EDGES({2, 3}, {0, 5}, {0, 1}, {2, 0}, {0, 3})},
    {.name = "edge list one field",
     .build = EDGE_LIST,
     .file = "0 1\n4\n",
     .vertices = 6,
     .ignore_self_loops = true,
     .rc = -1},
    {.name = "edge list source out of range",
     .build = EDGE_LIST,
     .file = "0 1\n6 0\n",
     .vertices = 6,
     .rc = -1},
    {.name = "edge list target out of range",
     .build = EDGE_LIST,
     .file = "0 1\n1 6\n",
     .vertices = 6,
     .rc = -1},
    {.name = "edge list 1-based out of range",
     .build = EDGE_LIST,
     .file = "% 1-based\n1 2\n7 1\n",
     .vertices = 6,
     .rc = -1},
    {.name = "edge list self loop out of range",
     .build = EDGE_LIST,
     .file = "0 1\n6 6\n",
     .vertices = 6,
     .ignore_self_loops = true,
     .rc = -1},
    /* src 0: {1, 2}, src 1: {0}, src 2: {3}, src 3: {0, 2}. */
    {.name = "adjacency",
     .build = ADJACENCY,
     .file = "4 6\n2 3\n1\n4\n1 3\n",
     .vertices = 4,
     .ignore_self_loops = true,
     EDGES({0, 1}, {0, 2}, {1, 0}, {2, 3}, {3, 0}, {3, 2})},
    /* Vertex 0's line "2<VT>3": the vertical tab separates two targets. */
    {.name = "adjacency vertical tab",
     .build = ADJACENCY,
     .file = "3 4\n2\v3\n1\n1\n",
     .vertices = 3,
     .ignore_self_loops = true,
     EDGES({0, 1}, {0, 2}, {1, 0}, {2, 0})},
    {.name = "adjacency zero token",
     .build = ADJACENCY,
     .file = "2 3\n0 2\n1\n",
     .vertices = 2,
     .ignore_self_loops = true,
     EDGES({0, 1}, {1, 0})},
    {.name = "adjacency flipped",
     .build = ADJACENCY,
     .file = "3 3\n2\n3\n1\n",
     .vertices = 3,
     .flip = true,
     .ignore_self_loops = true,
     EDGES({0, 1}, {1, 2}, {2, 0}, {1, 0}, {2, 1}, {0, 2})},
    /* Targets listed in descending order; vertex 1's line is empty. */
    {.name = "adjacency row order",
     .build = ADJACENCY,
     .file = "3 4\n3 2\n\n2 1\n",
     .vertices = 3,
     .ignore_self_loops = true,
     EDGES({0, 2}, {0, 1}, {2, 1}, {2, 0})},
    /* The header claims four lines and two follow. */
    {.name = "adjacency short",
     .build = ADJACENCY,
     .file = "4 3\n2 3\n1\n",
     .vertices = 4,
     .ignore_self_loops = true,
     .rc = -1},
    {.name = "adjacency header without edge count",
     .build = ADJACENCY,
     .file = "4\n2\n",
     .vertices = 4,
     .ignore_self_loops = true,
     .rc = -1},
    {.name = "adjacency target out of range",
     .build = ADJACENCY,
     .file = "3 2\n2\n4\n1\n",
     .vertices = 3,
     .ignore_self_loops = true,
     .rc = -1},
    /* The file agrees with its own header but has more vertices than the
     * distribution. */
    {.name = "adjacency source out of range",
     .build = ADJACENCY,
     .file = "3 3\n2\n1\n1\n",
     .vertices = 2,
     .ignore_self_loops = true,
     .rc = -1},
    {.name = "args edge list",
     .build = ARGS,
     .file = "0 1\n1 2\n2 2\n3 0\n",
     .vertices = 4,
     .args = {"--file", "@"},
     .nargs = 2,
     EDGES({0, 1}, {1, 2}, {3, 0})},
    {.name = "args keep self loops",
     .build = ARGS,
     .file = "0 1\n1 2\n2 2\n3 0\n",
     .vertices = 4,
     .args = {"--file", "@", "--keep-self-loops"},
     .nargs = 3,
     EDGES({0, 1}, {1, 2}, {2, 2}, {3, 0})},
    {.name = "args flip",
     .build = ARGS,
     .file = "0 1\n1 2\n",
     .vertices = 4,
     .args = {"--file", "@", "--flip"},
     .nargs = 3,
     EDGES({0, 1}, {1, 2}, {1, 0}, {2, 1})},
    {.name = "args adjacency",
     .build = ARGS,
     .file = "4 4\n2\n3\n3\n1\n",
     .vertices = 4,
     .args = {"--file", "@", "--csr-format"},
     .nargs = 3,
     EDGES({0, 1}, {1, 2}, {3, 0})},
    {.name = "args no file", .build = ARGS, .vertices = 4, .rc = -1},
    /* Sized exactly: a flag's value would be one past the end. */
    {.name = "args trailing file flag",
     .build = ARGS,
     .vertices = 4,
     .args = {"--file"},
     .nargs = 1,
     .rc = -1},
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))
#define MAX_DEGREE 16

/* Built in init_per_node, before parallel start; read only by EDTs. */
static arts_block_dist_t *g_dist[NCASES];

static void fail(const csr_case_t *c, const char *what) {
  arts_printf("FAIL: csr_graph %s: %s\n", c->name, what);
  arts_test_fail();
}

static int vertex_cmp(const void *a, const void *b) {
  arts_vertex_t x = *(const arts_vertex_t *)a;
  arts_vertex_t y = *(const arts_vertex_t *)b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

void init_per_node(unsigned int node_id, int argc, char **argv) {
  (void)node_id;
  (void)argc;
  (void)argv;
  unsigned int parts = 2 * arts_get_total_ranks();
  for (unsigned int i = 0; i < NCASES; ++i) {
    if (cases[i].build == DIRECT) {
      char *counts[] = {(char *)"prog", (char *)"--num-vertices", (char *)"8",
                        (char *)"--num-edges", (char *)"9"};
      g_dist[i] = arts_block_dist_init_from_args(5, counts);
    } else {
      g_dist[i] = arts_block_dist_init(cases[i].vertices, cases[i].nedges,
                                       parts, ARTS_GUID_DB);
    }
  }
}

static bool write_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return false;
  }
  fputs(content, f);
  fclose(f);
  return true;
}

/* Each local partition of a direct case, built from the edges whose source it
 * holds; a partition homed on another rank builds nothing. */
static void build_direct(const csr_case_t *c, arts_block_dist_t *dist) {
  for (arts_partition_t p = 0; p < dist->num_blocks; ++p) {
    arts_guid_t g = arts_block_dist_guid_for_partition(dist, p);
    arts_edge_vector_t vec;
    /* A zero-capacity vector has no array at all, so a sort or a build that
     * touched it would fault. */
    arts_edge_vector_init(&vec, (c->build == DIRECT) ? 16 : 0);
    if (!arts_guid_is_local(g)) {
      if (arts_csr_init(p, arts_block_dist_block_size(p, dist), 0, dist, &vec,
                        true, g) != NULL) {
        fail(c, "a build for a remote partition returned a CSR");
      }
      arts_edge_vector_free(&vec);
      continue;
    }
    for (unsigned int e = 0; e < c->nedges; ++e) {
      if (arts_block_dist_get_owner(c->edges[e].s, dist) == p) {
        arts_edge_vector_push_back(&vec, c->edges[e].s, c->edges[e].t, 0);
      }
    }
    arts_edge_vector_sort_by_source_and_target(&vec);
    arts_csr_graph_t *csr = arts_csr_init(
        p, arts_block_dist_block_size(p, dist), vec.used, dist, &vec, true, g);
    arts_edge_vector_free(&vec);
    if (csr == NULL) {
      fail(c, "a build for a local partition returned NULL");
    }
  }
}

static int load(const csr_case_t *c, arts_block_dist_t *dist,
                const char *path) {
  switch (c->build) {
  case EDGE_LIST:
    return arts_csr_load_no_weight(path, dist, c->flip, c->ignore_self_loops);
  case ADJACENCY:
    return arts_csr_load_no_weight_csr(path, dist, c->flip,
                                       c->ignore_self_loops);
  default: {
    char *argv[4] = {(char *)"prog"};
    for (int i = 0; i < c->nargs; ++i) {
      argv[i + 1] =
          (strcmp(c->args[i], "@") == 0) ? (char *)path : (char *)c->args[i];
    }
    return arts_csr_load_from_args(dist, c->nargs + 1, argv);
  }
  }
}

/* A partition a refused load did not create holds a stand-in whose header
 * names the complement of its GUID, so its readers find a block either way
 * and can tell which one they found. */
static void create_stand_ins(arts_block_dist_t *dist) {
  for (arts_partition_t p = 0; p < dist->num_blocks; ++p) {
    arts_guid_t g = arts_block_dist_guid_for_partition(dist, p);
    if (!arts_guid_is_local(g)) {
      continue;
    }
    arts_csr_graph_t *stand_in = (arts_csr_graph_t *)arts_db_create_with_guid(
        g, sizeof(arts_csr_graph_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    memset(stand_in, 0, sizeof(*stand_in));
    stand_in->partGuid = ~g;
  }
}

/* The loader of one rank: every case's partitions homed here. */
void load_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  for (unsigned int i = 0; i < NCASES; ++i) {
    const csr_case_t *c = &cases[i];
    if (c->build == DIRECT || c->build == DIRECT_EMPTY) {
      build_direct(c, g_dist[i]);
      continue;
    }
    char path[256];
    snprintf(path, sizeof(path), "arts_csr_graph_%u_%d.txt", i, (int)getpid());
    if (c->file != NULL && !write_file(path, c->file)) {
      fail(c, "cannot write the fixture");
      create_stand_ins(g_dist[i]);
      continue;
    }
    int rc = load(c, g_dist[i], path);
    if (c->file != NULL) {
      remove(path);
    }
    if (rc != c->rc) {
      fail(c, (c->rc == 0) ? "the load was refused" : "the load was accepted");
    }
    if (rc != 0) {
      create_stand_ins(g_dist[i]);
    }
  }
}

static void check_csr(const csr_case_t *c, arts_block_dist_t *dist,
                      arts_partition_t p, arts_csr_graph_t *csr) {
  arts_graph_sz_t size = arts_block_dist_block_size(p, dist);
  arts_vertex_t start = arts_block_dist_partition_start(p, dist);
  arts_graph_sz_t local_edges = 0;
  for (unsigned int e = 0; e < c->nedges; ++e) {
    if (arts_block_dist_get_owner(c->edges[e].s, dist) == p) {
      local_edges++;
    }
  }
  if (csr->index != p || csr->num_blocks != dist->num_blocks ||
      csr->num_local_vertices != size || csr->num_local_edges != local_edges) {
    fail(c, "header counts");
    return;
  }
  arts_vertex_t *prev = NULL;
  arts_graph_sz_t prev_cnt = 0;
  for (arts_vertex_t v = start; v < start + size; ++v) {
    arts_vertex_t want[MAX_DEGREE];
    arts_graph_sz_t n = 0;
    for (unsigned int e = 0; e < c->nedges; ++e) {
      if (c->edges[e].s == v && n < MAX_DEGREE) {
        want[n++] = c->edges[e].t;
      }
    }
    qsort(want, (size_t)n, sizeof(want[0]), vertex_cmp);
    arts_vertex_t *nbrs = NULL;
    arts_graph_sz_t cnt = (arts_graph_sz_t)-1;
    arts_csr_get_neighbors(csr, v, &nbrs, &cnt);
    if (v > start && nbrs != prev + prev_cnt) {
      fail(c, "a neighbour range does not start where the previous one ends");
      return;
    }
    if (cnt != n) {
      fail(c, "degree");
      return;
    }
    for (arts_graph_sz_t k = 0; k < cnt; ++k) {
      if (nbrs[k] != want[k]) {
        fail(c, "a row's targets are not the expected ascending sequence");
        return;
      }
    }
    if (start + (arts_vertex_t)arts_csr_get_local_index(v, csr) != v) {
      fail(c, "local index does not map back to its vertex");
      return;
    }
    prev = nbrs;
    prev_cnt = cnt;
  }
}

/* paramv: the case index and the partition.  Slot 0: the partition, RO. */
void read_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const csr_case_t *c = &cases[paramv[0]];
  arts_block_dist_t *dist = g_dist[paramv[0]];
  arts_partition_t p = (arts_partition_t)paramv[1];
  arts_guid_t g = arts_block_dist_guid_for_partition(dist, p);
  arts_csr_graph_t *csr = (arts_csr_graph_t *)depv[0].ptr;
  if (csr == NULL) {
    fail(c, "the partition's slot has no payload");
  } else if (csr->partGuid == ~g) {
    if (c->rc == 0) {
      fail(c, "the partition was not built");
    }
  } else if (csr->partGuid != g) {
    fail(c, "the partition's header names another GUID");
  } else if (c->rc != 0) {
    fail(c, "a refused load built a partition");
  } else {
    check_csr(c, dist, p, csr);
  }
}

void destroy_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  for (unsigned int i = 0; i < NCASES; ++i) {
    for (arts_partition_t p = 0; p < g_dist[i]->num_blocks; ++p) {
      arts_db_destroy(arts_block_dist_guid_for_partition(g_dist[i], p));
    }
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS csr_graph\n");
  }
  arts_shutdown();
}

/* Every rank reads every partition of every case. */
void read_phase_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t readers = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.finish_event = readers;
  for (unsigned int i = 0; i < NCASES; ++i) {
    for (arts_partition_t p = 0; p < g_dist[i]->num_blocks; ++p) {
      uint64_t pv[2] = {i, p};
      for (unsigned int r = 0; r < arts_get_total_ranks(); ++r) {
        h.rank = r;
        arts_guid_t reader = arts_edt_create(read_edt, 2, pv, 1, &h);
        arts_add_dependence(arts_block_dist_guid_for_partition(g_dist[i], p),
                            reader, 0, DB_MODE_RO);
      }
    }
  }
  arts_edt_hint_t here = ARTS_EDT_HINT_DEFAULTS;
  arts_guid_t done = arts_edt_create(destroy_edt, 0, NULL, 1, &here);
  arts_add_dependence(readers, done, 0, DB_MODE_NULL);
}

static void check_dist_args(void) {
  static const csr_case_t args_case = {.name = "distribution arguments"};
  char *no_counts[] = {(char *)"prog"};
  char *no_edges[] = {(char *)"prog", (char *)"--num-vertices", (char *)"16"};
  arts_block_dist_t *d = arts_block_dist_init_from_args(1, no_counts);
  if (d == NULL) {
    d = arts_block_dist_init_from_args(3, no_edges);
  }
  if (d != NULL) {
    fail(&args_case, "a distribution was built without both counts");
    arts_block_dist_free(d);
  }
  /* The vertex count's flag is the last argument; the token after it lies
   * past argc, so a parser that reads it builds a distribution of 7
   * vertices. */
  char *past_end[] = {(char *)"prog", (char *)"--num-edges", (char *)"4",
                      (char *)"--num-vertices", (char *)"7"};
  d = arts_block_dist_init_from_args(4, past_end);
  if (d != NULL) {
    fail(&args_case,
         "a trailing --num-vertices read a value past the arguments");
    arts_block_dist_free(d);
  }
  arts_block_dist_t *built = g_dist[0];
  if (built == NULL || built->num_vertices != 8 || built->num_edges != 9 ||
      built->num_blocks != arts_get_total_ranks()) {
    fail(&args_case, "the distribution from arguments has the wrong shape");
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  check_dist_args();
  arts_guid_t loaders = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.finish_event = loaders;
  for (unsigned int r = 0; r < arts_get_total_ranks(); ++r) {
    h.rank = r;
    arts_edt_create(load_edt, 0, NULL, 0, &h);
  }
  arts_edt_hint_t here = ARTS_EDT_HINT_DEFAULTS;
  arts_guid_t next = arts_edt_create(read_phase_edt, 0, NULL, 1, &here);
  arts_add_dependence(loaders, next, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
