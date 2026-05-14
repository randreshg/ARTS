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

// Global array to collect tile sums (set by main_edt, read by validate_edt)
uint64_t *tile_sums;

static void validate_edt(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  // Actual sum (serial)
  uint64_t actual_sum = 0;
  for (uint64_t i = 0; i < num_elems; i++) {
    actual_sum += *(elems[i]);
  }
  actual_sum *= num_iters;
  // Parallel sum (collected from tile_sums array)
  uint64_t parallel_sum = 0;
  for (uint32_t i = 0; i < total_tiles; i++) {
    parallel_sum += tile_sums[i];
  }
  if (actual_sum == parallel_sum) {
    arts_printf("PASSED\n");
  } else {
    arts_printf("FAILED!. Actual sum: %llu, Parallel sum: %llu\n", actual_sum, parallel_sum);
  }
  free(tile_sums);
  free(elem_guids);
  free(elems);
  arts_shutdown();
}

static void tile_sum_edt(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t tile_sum = 0;
  uint32_t tile = (uint32_t) paramv[0];
  arts_guid_t completion_event = (arts_guid_t) paramv[1];
  uint32_t iters = (uint32_t) paramv[2];
  for (uint32_t i = 0; i < iters; i++) {
    for (uint64_t j = 0; j < depc; j++) {
      tile_sum += *((uint64_t*) depv[j].ptr);
    }
  }
  // Store result in global tile_sums array (CXL-shared)
  tile_sums[tile] = tile_sum;
  // Signal the completion event (decrement latch counter)
  arts_event_satisfy_slot(completion_event, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

void main_edt(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
  num_elems = N;
  num_iters = ITER;
  total_tiles = arts_get_total_nodes() * arts_get_total_workers();

  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];

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
  }

  while (num_elems % total_tiles) {
    num_elems++;
  }

  elems_per_tile = num_elems / total_tiles;

  arts_printf("N: %llu, num tiles: %lu, num iterations: %lu\n", num_elems, total_tiles, num_iters);
  elem_guids = malloc(sizeof(arts_guid_t) * num_elems);
  elems = malloc(sizeof(uint64_t*) * num_elems);

  /* seed with current time */
  splitmix64_seed((uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&splitmix64_state);

  // Create, fill, release elem DBs (arts_db_release implicitly flushes)
  for (uint64_t i = 0; i < num_elems; i++) {
    elem_guids[i] = arts_db_create((void**) &(elems[i]), sizeof(uint64_t), ARTS_DB_CXL, NULL);
    *(elems[i]) = splitmix64_next();
    arts_db_release(elem_guids[i]);
    // arts_cxl_producer_flush(elem_guids[i]);
  }

  // Allocate tile_sums array (CXL-shared so all nodes can write to it)
  tile_sums = calloc(total_tiles, sizeof(uint64_t));

  // Create validate EDT with 1 dependency slot (will be satisfied by the event)
  arts_guid_t validate_edt_guid = arts_edt_create(validate_edt, 0, NULL, 1, &(arts_hint_t){.route = 0});

  // Create a latch event with latch_count = total_tiles
  // When all tile_sum_edts signal it, the event fires and triggers validate_edt
  arts_guid_t completion_event = arts_event_create(0, ARTS_EVENT_LATCH, total_tiles, NULL_GUID);

  // Wire the event to the validate EDT's slot 0
  arts_add_dependence(completion_event, validate_edt_guid, 0, DB_MODE_RO);

  uint32_t next = (arts_get_current_node() + 1) % arts_get_total_nodes();
  uint64_t global_count = 0;
  for (uint32_t i = 0; i < total_tiles; i++) {
    // Pass completion_event (not validate_edt_guid) so tile_sum_edt can signal it
    uint64_t args[] = {i, completion_event, num_iters};
    arts_guid_t ts_edt = arts_edt_create(tile_sum_edt, 3, args, elems_per_tile, &(arts_hint_t){.route = next});

    // Use arts_add_dependence to connect element DBs to tile_sum_edt
    uint64_t elem_count = 0;
    while (elem_count < elems_per_tile) {
      arts_add_dependence(elem_guids[global_count], ts_edt, elem_count, DB_MODE_RO);
      elem_count += 1;
      global_count += 1;
    }
    next = (next + 1) % arts_get_total_nodes();
  }
}

void init_per_node(unsigned int node_id, int argc, char **argv) {
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

