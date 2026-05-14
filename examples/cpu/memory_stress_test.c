/*****
 * Copyright 2019 Battelle Memorial Institute
 * Licensed under the Apache License, Version 2.0
 */

#include "arts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#define N 1000000000ULL
#define ITER 100000
uint64_t num_elems;
uint32_t num_iters;
uint32_t total_tiles;
uint64_t elems_per_tile;
uint64_t **elems;
arts_guid_t *elem_guids;
arts_guid_t validate_guid;
static uint64_t splitmix64_state;

/* Call once at startup to seed the generator. */
static void splitmix64_seed(uint64_t seed) {
  splitmix64_state = seed;
}

/* Returns next pseudorandom 64-bit integer. */
static uint64_t splitmix64_next(void) {
  uint64_t z = (splitmix64_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static void validate_edt(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  // Actual sum (serial)
  uint64_t actual_sum = 0;
  for (uint64_t i = 0; i < num_elems; i++) {
    actual_sum += *(elems[i]);
  }
  actual_sum *= num_iters;
  // Parallel sum
  uint64_t parallel_sum = 0;
  for (uint64_t i = 0; i < depc; i++) {
    parallel_sum += *((uint64_t*) depv[i].ptr);
  }
  if (actual_sum == parallel_sum)
    arts_printf("PASSED\n");
  else
    arts_printf("FAILED!. Actual sum: %llu, Parallel sum: %llu\n", actual_sum, parallel_sum);
  free(elem_guids);
  free(elems);
  arts_shutdown();
}

static void tile_sum_edt(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  uint64_t *tile_sum;
  arts_guid_t tile_sum_guid = arts_db_create((void**) &tile_sum, sizeof(uint64_t), ARTS_DB_CXL, NULL);
  *tile_sum = 0;
  uint32_t tile = (uint32_t) paramv[0];
  arts_guid_t val_guid = (arts_guid_t) paramv[1];
  uint32_t iters = (uint32_t) paramv[2];
  for (uint32_t i = 0; i < iters; i++) {
    for (uint64_t i = 0; i < depc; i++) {
      *tile_sum += *((uint64_t*) depv[i].ptr);
    }
    arts_cxl_producer_flush(tile_sum_guid);
  }
  arts_signal_edt(val_guid, tile, tile_sum_guid, DB_MODE_RO);
}

void init_per_node(unsigned int node_id, int argc, char **argv) {
  if (!node_id) {
    num_elems = N;
    num_iters = ITER;
    total_tiles = arts_get_total_nodes() * arts_get_total_workers();
    if (argc > 1) {
      num_elems = atol(argv[1]);
      if (sscanf(argv[1], "%" SCNu64, &num_elems) != 1) {
        fprintf(stderr, "invalid 64-bit unsigned integer: %s\n", argv[1]);
        arts_shutdown();
      }
      if (argc > 2) {
        if (sscanf(argv[2], "%" SCNu32, &num_iters) != 1) {
          fprintf(stderr, "invalid 32-bit unsigned integer: %s\n", argv[2]);
          arts_shutdown();
        }
      }

      while (num_elems % total_tiles) {
        num_elems++;
      }

      elems_per_tile = num_elems/total_tiles;

      arts_printf("N: %llu, num tiles: %lu, num iterations: %lu\n", num_elems, total_tiles, num_iters);
      elem_guids = malloc(sizeof(arts_guid_t) * num_elems);
      elems = malloc(sizeof(uint64_t*) * num_elems);

      /* seed with current time */
      splitmix64_seed((uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&splitmix64_state);

      // Create, fill, flush elem DBs
      for (uint64_t i = 0; i < num_elems; i++) {
        elem_guids[i] = arts_db_create((void**) &(elems[i]), sizeof(uint64_t), ARTS_DB_CXL, NULL);
        *(elems[i]) = splitmix64_next();
        arts_cxl_producer_flush(elem_guids[i]);
      }

      // Validate EDT
      validate_guid = arts_edt_create(validate_edt, 0, NULL, total_tiles, &(arts_hint_t){.route = 0});

      uint64_t db_idx = 0;
      uint32_t next = (arts_get_current_node() + 1) % arts_get_total_nodes();
      uint64_t global_count = 0;
      for (uint32_t i = 0; i < total_tiles; i++) {
        uint64_t args[] = {i, validate_guid, num_iters};
        arts_guid_t ts_edt = arts_edt_create(tile_sum_edt, 3, args, elems_per_tile, &(arts_hint_t){.route = next});
        uint64_t elem_count = 0;
        while (elem_count < elems_per_tile) {
          arts_signal_edt(ts_edt, elem_count, elem_guids[global_count], DB_MODE_RO);
          elem_count += 1;
          global_count += 1;
        }
        next = (next+1) % arts_get_total_nodes();
      }
    }
  }
}

void init_per_worker(unsigned int node_id, unsigned int worker_id,
                     int argc, char **argv) {
}

/*
 * Main entry point
 */
int main(int argc, char **argv) {
    arts_rt(argc, argv);
    return 0;
}
