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
/// @brief The graph library on one rank: block distribution, CSR build and
///        lookup, the two file loaders and their argument dispatch.
///
/// Each case checks its own oracle and reports a failure through
/// arts_test_fail(), so one run states every property below:
///   * a CSR built from edges is well formed: every local vertex's neighbour
///     range starts where the previous one ends, vertices with no out-edges
///     included, the degrees sum to the local edge count, each range holds
///     exactly the edges fed in, and a vertex's local index maps back to it
///     through its partition's start;
///   * a partition with no edges builds an empty CSR without touching the
///     (empty) edge array;
///   * a CSR is found by its GUID and by its partition, the same pointer on
///     every one of many lookups (a lookup takes and drops a reference each
///     time), and a GUID that names no CSR finds nothing;
///   * the edge-list loader skips '%' headers and the line after them and '#'
///     comments, drops self loops when asked, survives a line with extra
///     tokens, and adds every reverse edge when flipping;
///   * the 1-based adjacency loader decrements targets, builds nothing when
///     the header disagrees with the line count, and skips a 0 token, which
///     has no 0-based vertex;
///   * argument dispatch selects the loader and its options, refuses a missing
///     path, and never reads a flag's value past the end of the argument
///     array, for the loaders and for the distribution constructor alike.
///
/// CSR blocks are pinned blocks, so no coherence protocol is involved.  One
/// rank: the loaders and the lookups assert on the rank's own partitions.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "arts.h"
#include "arts/graph.h"

#include "../test_failure_status.h"

static void fail(const char *what) {
  arts_printf("FAIL: csr_graph %s\n", what);
  arts_test_fail();
}

static int vertex_cmp(const void *a, const void *b) {
  arts_vertex_t x = *(const arts_vertex_t *)a;
  arts_vertex_t y = *(const arts_vertex_t *)b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
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

static bool has_neighbor(arts_block_dist_t *dist, arts_vertex_t v,
                         arts_vertex_t target) {
  arts_csr_graph_t *csr = arts_csr_from_partition(0, dist);
  if (csr == NULL) {
    return false;
  }
  arts_vertex_t *nbrs = NULL;
  arts_graph_sz_t cnt = 0;
  arts_csr_get_neighbors(csr, v, &nbrs, &cnt);
  for (arts_graph_sz_t k = 0; k < cnt; ++k) {
    if (nbrs[k] == target) {
      return true;
    }
  }
  return false;
}

static void free_partition0(arts_block_dist_t *dist) {
  arts_csr_graph_t *csr = arts_csr_from_partition(0, dist);
  if (csr != NULL) {
    arts_csr_free(csr);
  }
}

/* Eight vertices; 2, 5 and 6 have no out-edges (interior gaps, one just before
 * the last source). */
static void case_wellformed(void) {
  static const arts_vertex_t src[] = {0, 1, 1, 3, 3, 3, 4, 7, 7};
  static const arts_vertex_t tgt[] = {3, 0, 4, 2, 5, 7, 1, 0, 6};
  static const arts_graph_sz_t degree[] = {1, 2, 0, 3, 1, 0, 0, 2};
  const int nedges = (int)(sizeof(src) / sizeof(src[0]));
  const arts_vertex_t nverts = 8;

  arts_block_dist_t *dist =
      arts_block_dist_init(nverts, (arts_graph_sz_t)nedges, 1, ARTS_GUID_DB);
  arts_edge_vector_t vec;
  arts_edge_vector_init(&vec, 64);
  for (int i = 0; i < nedges; ++i) {
    arts_edge_vector_push_back(&vec, src[i], tgt[i], 0);
  }
  arts_edge_vector_sort_by_source(&vec);
  arts_csr_graph_t *csr =
      arts_csr_init(0, nverts, (arts_graph_sz_t)nedges, dist, &vec, true,
                    arts_block_dist_guid_for_partition(dist, 0));
  arts_edge_vector_free(&vec);
  if (csr == NULL) {
    fail("wellformed: init returned NULL");
    arts_block_dist_free(dist);
    return;
  }
  if (csr->num_local_vertices != nverts ||
      csr->num_local_edges != (arts_graph_sz_t)nedges) {
    fail("wellformed: header counts");
  }

  arts_vertex_t pstart = arts_block_dist_partition_start(0, dist);
  arts_vertex_t *prev = NULL;
  arts_graph_sz_t prev_cnt = 0;
  arts_graph_sz_t total = 0;
  for (arts_vertex_t v = 0; v < nverts; ++v) {
    arts_vertex_t *nbrs = NULL;
    arts_graph_sz_t cnt = (arts_graph_sz_t)-1;
    arts_csr_get_neighbors(csr, v, &nbrs, &cnt);
    if (cnt != degree[v]) {
      fail("wellformed: degree");
      break;
    }
    if (v > 0 && nbrs != prev + prev_cnt) {
      fail("wellformed: a neighbour range does not start where the previous "
           "one ends");
      break;
    }
    if (cnt > 0) {
      arts_vertex_t want[8];
      arts_vertex_t got[8];
      int n = 0;
      for (int e = 0; e < nedges; ++e) {
        if (src[e] == v) {
          want[n++] = tgt[e];
        }
      }
      for (arts_graph_sz_t k = 0; k < cnt; ++k) {
        got[k] = nbrs[k];
      }
      qsort(want, (size_t)n, sizeof(want[0]), vertex_cmp);
      qsort(got, (size_t)cnt, sizeof(got[0]), vertex_cmp);
      for (arts_graph_sz_t k = 0; k < cnt; ++k) {
        if (got[k] != want[k]) {
          fail("wellformed: neighbour set");
          break;
        }
      }
    }
    if (pstart + (arts_vertex_t)arts_csr_get_local_index(v, csr) != v) {
      fail("wellformed: local index does not map back to its vertex");
    }
    prev = nbrs;
    prev_cnt = cnt;
    total += cnt;
  }
  if (total != (arts_graph_sz_t)nedges) {
    fail("wellformed: degrees do not sum to the local edge count");
  }
  arts_csr_free(csr);
  arts_block_dist_free(dist);
}

/* A zero-capacity edge vector has no array at all, so a build that touched it
 * would fault. */
static void case_empty_partition(void) {
  arts_block_dist_t *dist = arts_block_dist_init(8, 0, 1, ARTS_GUID_DB);
  arts_edge_vector_t vec;
  arts_edge_vector_init(&vec, 0);
  arts_csr_graph_t *csr = arts_csr_init(
      0, 8, 0, dist, &vec, true, arts_block_dist_guid_for_partition(dist, 0));
  arts_edge_vector_free(&vec);
  if (csr == NULL) {
    fail("empty partition: init returned NULL");
    arts_block_dist_free(dist);
    return;
  }
  if (csr->num_local_edges != 0 || csr->num_local_vertices != 8) {
    fail("empty partition: header counts");
  }
  for (arts_vertex_t v = 0; v < 8; ++v) {
    arts_vertex_t *nbrs = NULL;
    arts_graph_sz_t cnt = (arts_graph_sz_t)-1;
    arts_csr_get_neighbors(csr, v, &nbrs, &cnt);
    if (cnt != 0) {
      fail("empty partition: a vertex reports neighbours");
      break;
    }
  }
  arts_csr_free(csr);
  arts_block_dist_free(dist);
}

#define LOOKUPS 2048

static void case_lookup(void) {
  arts_block_dist_t *dist = arts_block_dist_init(64, 3, 1, ARTS_GUID_DB);
  arts_edge_vector_t vec;
  arts_edge_vector_init(&vec, 16);
  arts_edge_vector_push_back(&vec, 0, 1, 0);
  arts_edge_vector_push_back(&vec, 0, 2, 0);
  arts_edge_vector_push_back(&vec, 1, 0, 0);
  arts_edge_vector_sort_by_source(&vec);
  arts_guid_t g0 = arts_block_dist_guid_for_partition(dist, 0);
  arts_csr_graph_t *built = arts_csr_init(
      0, arts_block_dist_block_size(0, dist), vec.used, dist, &vec, true, g0);
  arts_edge_vector_free(&vec);
  if (built == NULL) {
    fail("lookup: init returned NULL");
    arts_block_dist_free(dist);
    return;
  }
  for (int i = 0; i < LOOKUPS; ++i) {
    arts_csr_graph_t *got = arts_csr_from_guid(g0);
    /* Reading a field of the descriptor faults under a sanitizer if a lookup
     * dropped one reference too many. */
    if (got != built || got->partGuid != g0) {
      fail("lookup: a repeated lookup found another CSR");
      break;
    }
  }
  if (arts_csr_from_partition(0, dist) != built) {
    fail("lookup: the partition lookup disagrees with the GUID lookup");
  }
  arts_guid_t never = arts_guid_reserve(ARTS_GUID_DB, arts_get_current_rank());
  if (arts_csr_from_guid(never) != NULL) {
    fail("lookup: a GUID that names no CSR found one");
  }
  arts_csr_free(built);
  arts_block_dist_free(dist);
}

static void case_load_edgelist(void) {
  char path[256];
  snprintf(path, sizeof(path), "arts_csr_graph_edgelist_%d.txt", (int)getpid());
  /* An mmio '%' header and the dimension line after it, a '#' comment, a self
   * loop, and a line with a third token. */
  if (!write_file(path, "% MatrixMarket header line\n"
                        "6 6 7\n"
                        "# a comment line\n"
                        "0 1\n"
                        "0 2\n"
                        "1 0\n"
                        "3 3\n"
                        "4 5 99\n"
                        "5 0\n")) {
    fail("edgelist: cannot write the fixture");
    return;
  }

  arts_block_dist_t *dist = arts_block_dist_init(6, 0, 1, ARTS_GUID_DB);
  if (arts_csr_load_no_weight(path, dist, false, true) != 0) {
    fail("edgelist: load failed");
  } else if (!has_neighbor(dist, 0, 1) || !has_neighbor(dist, 0, 2) ||
             !has_neighbor(dist, 1, 0) || !has_neighbor(dist, 5, 0)) {
    fail("edgelist: an edge is missing");
  } else if (has_neighbor(dist, 3, 3)) {
    fail("edgelist: a self loop was kept");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);

  dist = arts_block_dist_init(6, 0, 1, ARTS_GUID_DB);
  if (arts_csr_load_no_weight(path, dist, true, true) != 0) {
    fail("edgelist: flipped load failed");
  } else if (!has_neighbor(dist, 2, 0)) {
    fail("edgelist: the flipped load lacks the reverse edge 2->0");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);
  remove(path);
}

static void case_load_adjacency(void) {
  char path[256];

  /* src 0: {1, 2}, src 1: {0}, src 2: {3}, src 3: {0, 2}. */
  snprintf(path, sizeof(path), "arts_csr_graph_adj_%d.txt", (int)getpid());
  if (!write_file(path, "4 6\n2 3\n1\n4\n1 3\n")) {
    fail("adjacency: cannot write the fixture");
    return;
  }
  arts_block_dist_t *dist = arts_block_dist_init(4, 0, 1, ARTS_GUID_DB);
  if (arts_csr_load_no_weight_csr(path, dist, false, true) != 0) {
    fail("adjacency: load failed");
  } else if (!has_neighbor(dist, 0, 1) || !has_neighbor(dist, 0, 2) ||
             !has_neighbor(dist, 1, 0) || !has_neighbor(dist, 2, 3) ||
             !has_neighbor(dist, 3, 0) || !has_neighbor(dist, 3, 2)) {
    fail("adjacency: neighbours are not the decremented targets");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);
  remove(path);

  /* The header claims four lines and two follow. */
  snprintf(path, sizeof(path), "arts_csr_graph_short_%d.txt", (int)getpid());
  if (!write_file(path, "4 3\n2 3\n1\n")) {
    fail("adjacency: cannot write the short fixture");
    return;
  }
  dist = arts_block_dist_init(4, 0, 1, ARTS_GUID_DB);
  if (arts_csr_load_no_weight_csr(path, dist, false, true) != 0) {
    fail("adjacency: a short file did not load as nothing");
  } else if (arts_csr_from_partition(0, dist) != NULL) {
    fail("adjacency: a short file built a CSR");
  }
  arts_block_dist_free(dist);
  remove(path);

  snprintf(path, sizeof(path), "arts_csr_graph_zero_%d.txt", (int)getpid());
  if (!write_file(path, "2 3\n0 2\n1\n")) {
    fail("adjacency: cannot write the zero-token fixture");
    return;
  }
  dist = arts_block_dist_init(2, 0, 1, ARTS_GUID_DB);
  (void)arts_csr_load_no_weight_csr(path, dist, false, true);
  if (has_neighbor(dist, 0, (arts_vertex_t)-1)) {
    fail("adjacency: a 0 token became a neighbour");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);
  remove(path);
}

static void case_args(void) {
  char el_path[256];
  char adj_path[256];
  snprintf(el_path, sizeof(el_path), "arts_csr_graph_args_el_%d.txt",
           (int)getpid());
  snprintf(adj_path, sizeof(adj_path), "arts_csr_graph_args_adj_%d.txt",
           (int)getpid());
  if (!write_file(el_path, "0 1\n1 2\n2 2\n3 0\n") ||
      !write_file(adj_path, "4 4\n2\n3\n3\n1\n")) {
    fail("args: cannot write the fixtures");
    remove(el_path);
    return;
  }

  arts_block_dist_t *dist = arts_block_dist_init(4, 0, 1, ARTS_GUID_DB);
  char *edgelist[] = {(char *)"prog", (char *)"--file", el_path};
  if (arts_csr_load_from_args(dist, 3, edgelist) != 0 ||
      !has_neighbor(dist, 0, 1)) {
    fail("args: the edge-list loader was not selected");
  } else if (has_neighbor(dist, 2, 2)) {
    fail("args: a self loop was kept by default");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);

  dist = arts_block_dist_init(4, 0, 1, ARTS_GUID_DB);
  char *keep[] = {(char *)"prog", (char *)"--file", el_path,
                  (char *)"--keep-self-loops"};
  if (arts_csr_load_from_args(dist, 4, keep) != 0 ||
      !has_neighbor(dist, 2, 2)) {
    fail("args: --keep-self-loops did not keep 2->2");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);

  dist = arts_block_dist_init(4, 0, 1, ARTS_GUID_DB);
  char *adjacency[] = {(char *)"prog", (char *)"--file", adj_path,
                       (char *)"--csr-format"};
  if (arts_csr_load_from_args(dist, 4, adjacency) != 0 ||
      !has_neighbor(dist, 0, 1)) {
    fail("args: --csr-format did not select the adjacency loader");
  }
  free_partition0(dist);
  arts_block_dist_free(dist);
  remove(el_path);
  remove(adj_path);

  dist = arts_block_dist_init(4, 0, 1, ARTS_GUID_DB);
  char *none[] = {(char *)"prog"};
  if (arts_csr_load_from_args(dist, 1, none) != -1) {
    fail("args: a load with no --file did not refuse");
  }
  /* Sized exactly: a flag's value would be one past the end. */
  char *trailing[] = {(char *)"prog", (char *)"--file"};
  if (arts_csr_load_from_args(dist, 2, trailing) != -1) {
    fail("args: a trailing --file read a value past the arguments");
  }
  arts_block_dist_free(dist);

  char *no_counts[] = {(char *)"prog"};
  char *no_edges[] = {(char *)"prog", (char *)"--num-vertices", (char *)"16"};
  arts_block_dist_t *d = arts_block_dist_init_from_args(1, no_counts);
  if (d == NULL) {
    d = arts_block_dist_init_from_args(3, no_edges);
  }
  if (d != NULL) {
    fail("args: a distribution was built without both counts");
    arts_block_dist_free(d);
  }
  /* The vertex count's flag is the last argument; the token after it lies
   * past argc, so a parser that reads it builds a distribution of 7
   * vertices. */
  char *past_end[] = {(char *)"prog", (char *)"--num-edges", (char *)"4",
                      (char *)"--num-vertices", (char *)"7"};
  d = arts_block_dist_init_from_args(4, past_end);
  if (d != NULL) {
    fail("args: a trailing --num-vertices read a value past the arguments");
    arts_block_dist_free(d);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  if (arts_get_total_ranks() != 1) {
    fail("needs exactly one rank");
    arts_shutdown();
    return;
  }
  case_wellformed();
  case_empty_partition();
  case_lookup();
  case_load_edgelist();
  case_load_adjacency();
  case_args();
  if (arts_test_status() == 0) {
    arts_printf("PASS csr_graph\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
