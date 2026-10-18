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
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>

#include "arts.h"

#define MATSIZE 3
#define TILE 1

uint64_t start = 0;

unsigned int num_blocks = 1;
arts_guid_t a_mat_guid = NULL_GUID;
arts_guid_t b_mat_guid = NULL_GUID;
arts_guid_t c_mat_guid = NULL_GUID;
arts_guid_t a_tile_guids = NULL_GUID;
arts_guid_t b_tile_guids = NULL_GUID;

void print_matrix(unsigned int row_size, float *mat) {
  unsigned int column_size = row_size;
  for (unsigned int i = 0; i < column_size; i++) {
    for (unsigned int j = 0; j < row_size; j++) {
      printf("%5.2f ", mat[(i * row_size) + j]);
    }
    printf("\n");
  }
}

void init_matrix(unsigned int row_size, float *mat, bool identity, bool zero) {
  unsigned int column_size = row_size;
  for (unsigned int i = 0; i < column_size; i++) {
    for (unsigned int j = 0; j < row_size; j++) {
      if (zero) {
        mat[(i * row_size) + j] = 0;
      } else if (identity) {
        if (i == j) {
          mat[(i * row_size) + j] = 1;
        } else {
          mat[(i * row_size) + j] = 0;
        }
      } else {
        mat[(i * row_size) + j] = (float)((i * row_size) + j);
      }
    }
  }
}

void copy_block(unsigned int x, unsigned int y, unsigned int tile_row_size,
                float *tile, unsigned int row_size, float *mat, bool to_tile) {
  unsigned int tile_column_size = tile_row_size;
  unsigned int column_size = row_size;

  unsigned int x_offset = tile_row_size * x;
  unsigned int y_offset = tile_column_size * y;

  if (to_tile) {
    for (unsigned int i = 0; i < tile_column_size; i++) {
      memcpy(&tile[(size_t)i * tile_row_size],
             &mat[((size_t)(i + y_offset) * row_size) + x_offset],
             (size_t)tile_row_size * sizeof(float));
    }
  } else {
    for (unsigned int i = 0; i < tile_column_size; i++) {
      memcpy(&mat[((size_t)(i + y_offset) * row_size) + x_offset],
             &tile[(size_t)i * tile_row_size],
             (size_t)tile_row_size * sizeof(float));
    }
  }
}

void init_block_mm(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  unsigned int i = paramv[0];
  unsigned int j = paramv[1];
  arts_guid_t init_latch = (arts_guid_t)paramv[2];

  //    arts_printf("%s %u %u\n", __func__, i, j);

  float *a_mat = (float *)depv[0].ptr;
  float *b_mat = (float *)depv[1].ptr;

  arts_guid_t a_guid = arts_guid_from_index(a_tile_guids, (i * num_blocks) + j);
  arts_guid_t b_guid = arts_guid_from_index(b_tile_guids, (i * num_blocks) + j);

  float *a_tile = (float *)arts_db_create_with_guid(
      a_guid, sizeof(float) * TILE * TILE, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  float *b_tile = (float *)arts_db_create_with_guid(
      b_guid, sizeof(float) * TILE * TILE, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  //    arts_db_create_with_guid_and_data(arts_guid_t guid, void * data,
  //    uint64_t size)

  copy_block(i, j, TILE, a_tile, MATSIZE, a_mat, true);
  copy_block(i, j, TILE, b_tile, MATSIZE, b_mat, true);
  arts_db_release(depv[0].guid, DB_MODE_RO);
  arts_db_release(depv[1].guid, DB_MODE_RO);
  arts_db_release(a_guid, DB_MODE_RW);
  arts_db_release(b_guid, DB_MODE_RW);
  arts_event_satisfy(init_latch, NULL_GUID);
}

void mm_kernel_cpu(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  unsigned int idx_i = paramv[4];
  unsigned int idx_j = paramv[5];
  arts_guid_t to_signal = (arts_guid_t)paramv[1];
  unsigned int idx_k = (unsigned int)paramv[2];
  //    arts_printf("%s %u %u %u %u SIG: %u\n", __func__, idx_i,idx_k,
  //    idx_k,idx_j);
  arts_guid_t c_tile_guid = (arts_guid_t)paramv[3];
  const unsigned int blk = (unsigned int)paramv[0];
  float *mat_a = (float *)depv[0].ptr;
  float *mat_b = (float *)depv[1].ptr;
  float *mat_c = (float *)depv[2].ptr;

  for (unsigned int i = 0; i < blk; i++) {
    // rows of B
    for (unsigned int j = 0; j < blk; j++) {
      // rows of A and columns of B
      for (unsigned int k = 0; k < blk; k++) {
        mat_c[(i * blk) + j] += mat_a[(i * blk) + k] * mat_b[(k * blk) + j];
      }
    }
  }
  arts_db_release(depv[0].guid, DB_MODE_RO);
  arts_db_release(depv[1].guid, DB_MODE_RO);
  arts_db_release(c_tile_guid, DB_MODE_RW);
  arts_add_dependence(c_tile_guid, to_signal, idx_k, DB_MODE_RO);
}

void multiply_mm(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  arts_guid_t to_signal = (arts_guid_t)paramv[0];

  unsigned int row_size = TILE;
  unsigned int column_size = TILE;

  unsigned int i = paramv[1];
  unsigned int j = paramv[2];
  unsigned int k = paramv[3];

  arts_printf("%s i: %u k: %u x k: %u j: %u %" PRIdPTR " %" PRIdPTR "\n",
              __func__, i, k, k, j, depv[0].guid, depv[1].guid);

  float *a_tile = (float *)depv[0].ptr;
  float *b_tile = (float *)depv[1].ptr;
  float *c_tile = NULL;

  arts_guid_t c_tile_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  c_tile = (float *)arts_db_create_with_guid(c_tile_guid,
                                             sizeof(float) * TILE * TILE,
                                             ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  init_matrix(row_size, c_tile, false, true);
  arts_db_release(c_tile_guid, DB_MODE_RW);

  uint64_t args[] = {TILE, (uint64_t)to_signal, k, (uint64_t)c_tile_guid, i, j,
                     k};
  arts_guid_t mul_gpu_guid =
      arts_edt_create(mm_kernel_cpu, 7, args, 3, &(arts_edt_hint_t){.rank = 0});
  arts_guid_t a_tile_guid = depv[0].guid;
  arts_guid_t b_tile_guid = depv[1].guid;
  arts_db_release(a_tile_guid, DB_MODE_RO);
  arts_db_release(b_tile_guid, DB_MODE_RO);
  arts_add_dependence(a_tile_guid, mul_gpu_guid, 0, DB_MODE_RO);
  arts_add_dependence(b_tile_guid, mul_gpu_guid, 1, DB_MODE_RO);
  arts_add_dependence(c_tile_guid, mul_gpu_guid, 2, DB_MODE_RW);
}

void sum_mm(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  arts_guid_t done_guid = (arts_guid_t)paramv[0];

  unsigned int row_size = TILE;
  unsigned int column_size = TILE;

  unsigned int idx_i = paramv[1];
  unsigned int idx_j = paramv[2];

  //    arts_printf("%s: i: %u j: %u %lu\n", __func__, idx_i, idx_j, done_guid);

  float *c_tile;
  arts_guid_t c_tile_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  c_tile = (float *)arts_db_create_with_guid(c_tile_guid,
                                             sizeof(float) * TILE * TILE,
                                             ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  init_matrix(row_size, c_tile, false, true);

  for (unsigned int i = 0; i < depc; i++) {
    float *to_add = (float *)depv[i].ptr;
    for (unsigned int j = 0; j < column_size; j++) {
      for (unsigned int k = 0; k < row_size; k++) {
        c_tile[(j * row_size) + k] += to_add[(j * row_size) + k];
      }
    }
    arts_db_destroy(depv[i].guid);
  }
  arts_db_release(c_tile_guid, DB_MODE_RW);
  arts_add_dependence(c_tile_guid, done_guid, 1 + (idx_i * num_blocks + idx_j),
                      DB_MODE_RO);
}

void finish_block_mm(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  arts_printf("%s\n", __func__);
  arts_guid_t init_latch = (arts_guid_t)paramv[0];
  float *c_mat = (float *)depv[0].ptr;
  for (unsigned int i = 0; i < num_blocks; i++) {
    for (unsigned int j = 0; j < num_blocks; j++) {
      float *c_tile = (float *)depv[1 + (i * num_blocks) + j].ptr;
      copy_block(i, j, TILE, c_tile, MATSIZE, c_mat, false);
    }
  }
  uint64_t time = arts_get_time_stamp() - start;
  //    print_matrix(MATSIZE, c_mat);
  arts_printf("DONE %lu\n", time);

  for (unsigned int i = 1; i < depc; i++) {
    arts_db_destroy(depv[i].guid);
  }
  for (unsigned int t = 0; t < num_blocks * num_blocks; t++) {
    arts_db_destroy(arts_guid_from_index(a_tile_guids, t));
    arts_db_destroy(arts_guid_from_index(b_tile_guids, t));
  }
  arts_db_destroy(a_mat_guid);
  arts_db_destroy(b_mat_guid);
  arts_db_destroy(c_mat_guid);
  arts_event_destroy(init_latch);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  num_blocks = MATSIZE / TILE;

  a_mat_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  b_mat_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  c_mat_guid = arts_guid_reserve(ARTS_GUID_DB, 0);

  a_tile_guids =
      arts_guid_reserve_range(ARTS_GUID_DB, num_blocks * num_blocks, 0);
  b_tile_guids =
      arts_guid_reserve_range(ARTS_GUID_DB, num_blocks * num_blocks, 0);

  float *a_mat = (float *)arts_db_create_with_guid(
      a_mat_guid, (unsigned long)MATSIZE * MATSIZE * sizeof(float), ARTS_DB,
      ARTS_DB_PROP_NONE, NULL);
  float *b_mat = (float *)arts_db_create_with_guid(
      b_mat_guid, (unsigned long)MATSIZE * MATSIZE * sizeof(float), ARTS_DB,
      ARTS_DB_PROP_NONE, NULL);
  float *c_mat = (float *)arts_db_create_with_guid(
      c_mat_guid, (unsigned long)MATSIZE * MATSIZE * sizeof(float), ARTS_DB,
      ARTS_DB_PROP_NONE, NULL);

  init_matrix(MATSIZE, a_mat, false, false);
  init_matrix(MATSIZE, b_mat, true, false);
  init_matrix(MATSIZE, c_mat, false, true);
  arts_db_release(a_mat_guid, DB_MODE_RW);
  arts_db_release(b_mat_guid, DB_MODE_RW);
  arts_db_release(c_mat_guid, DB_MODE_RW);

  //        arts_printf("A MATRIX\n");
  //        print_matrix(MATSIZE, a_mat);
  //        arts_printf("B MATRIX\n");
  //        print_matrix(MATSIZE, b_mat);
  //        arts_printf("C MATRIX\n");
  //        print_matrix(MATSIZE, c_mat);
  arts_printf("Starting\n");
  start = arts_get_time_stamp();

  arts_guid_t init_latch =
      arts_event_create(&ARTS_EVENT_HINT_LATCH(num_blocks * num_blocks));
  uint64_t done_args[] = {(uint64_t)init_latch};
  arts_guid_t done_guid = arts_edt_create(finish_block_mm, 1, done_args,
                                          1 + (num_blocks * num_blocks),
                                          &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(c_mat_guid, done_guid, 0, DB_MODE_RW);

  for (unsigned int i = 0; i < num_blocks; i++) {
    for (unsigned int j = 0; j < num_blocks; j++) {
      uint64_t init_args[] = {i, j, (uint64_t)init_latch};
      arts_guid_t init_guid = arts_edt_create(init_block_mm, 3, init_args, 2,
                                              &(arts_edt_hint_t){.rank = 0});
      arts_add_dependence(a_mat_guid, init_guid, 0, DB_MODE_RO);
      arts_add_dependence(b_mat_guid, init_guid, 1, DB_MODE_RO);

      uint64_t sum_args[] = {(uint64_t)done_guid, i, j};
      arts_guid_t sum_guid = arts_edt_create(sum_mm, 3, sum_args, num_blocks,
                                             &(arts_edt_hint_t){.rank = 0});
      arts_printf("SUMGUID: i: %u j: %u %" PRIdPTR "\n", i, j, sum_guid);
      for (unsigned int k = 0; k < num_blocks; k++) {
        uint64_t args[] = {(uint64_t)sum_guid, i, j, k};
        arts_guid_t mul_guid = arts_edt_create(multiply_mm, 4, args, 3,
                                               &(arts_edt_hint_t){.rank = 0});
        arts_printf("%" PRIdPTR " Signaling: i: %u k: %u %" PRIdPTR
                    " i: %u k: %u %" PRIdPTR "\n",
                    mul_guid, i, k,
                    arts_guid_from_index(a_tile_guids, (i * num_blocks) + k), k,
                    j,
                    arts_guid_from_index(b_tile_guids, (k * num_blocks) + j));
        arts_add_dependence(
            arts_guid_from_index(a_tile_guids, (i * num_blocks) + k), mul_guid,
            0, DB_MODE_RO);
        arts_add_dependence(
            arts_guid_from_index(b_tile_guids, (k * num_blocks) + j), mul_guid,
            1, DB_MODE_RO);
        arts_add_dependence(init_latch, mul_guid, 2, DB_MODE_NULL);
      }
    }
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
