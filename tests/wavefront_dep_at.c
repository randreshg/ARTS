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
******************************************************************************/

/// @file wavefront_dep_at.c
/// @brief Exercises Seidel-style whole-DB owner writes plus RO halo slices.

#include "arts.h"
#include <stdlib.h>

static void fail_test(const char *msg) {
  arts_printf("  FAIL: %s\n", msg);
  abort();
}

static void expect_pair(const char *label, int *ptr, int expected0,
                        int expected1) {
  if (!ptr)
    fail_test(label);
  if (ptr[0] != expected0 || ptr[1] != expected1) {
    arts_printf("  FAIL: %s got [%d, %d] expected [%d, %d]\n", label, ptr[0],
                ptr[1], expected0, expected1);
    abort();
  }
}

static void expect_quad(const char *label, int *ptr, int expected0,
                        int expected1, int expected2, int expected3) {
  if (!ptr)
    fail_test(label);
  if (ptr[0] != expected0 || ptr[1] != expected1 || ptr[2] != expected2 ||
      ptr[3] != expected3) {
    arts_printf("  FAIL: %s got [%d, %d, %d, %d] expected [%d, %d, %d, %d]\n",
                label, ptr[0], ptr[1], ptr[2], ptr[3], expected0, expected1,
                expected2, expected3);
    abort();
  }
}

static void expect_guid_mode(const char *label, arts_edt_dep_t dep,
                             arts_guid_t expected_guid,
                             arts_db_access_mode_t expected_mode) {
  if (dep.guid != expected_guid) {
    arts_printf("  FAIL: %s guid=%lu expected %lu\n", label,
                (unsigned long)dep.guid, (unsigned long)expected_guid);
    abort();
  }
  if (dep.mode != expected_mode) {
    arts_printf("  FAIL: %s mode=%u expected %u\n", label, dep.mode,
                expected_mode);
    abort();
  }
}

void check_left_halo_slice(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t expected_guid = (arts_guid_t)paramv[0];
  int *slice = (int *)depv[0].ptr;
  expect_guid_mode("left halo RO slice", depv[0], expected_guid, DB_MODE_PTR);
  expect_pair("left halo payload", slice, 30, 40);
  arts_printf("  PASS: left halo RO slice delivered as DB_MODE_PTR\n");
}

void apply_center_wavefront_step(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t center_guid = (arts_guid_t)paramv[0];
  arts_guid_t left_guid = (arts_guid_t)paramv[1];
  arts_guid_t right_guid = (arts_guid_t)paramv[2];
  int *center = (int *)depv[0].ptr;
  int *left_halo = (int *)depv[1].ptr;
  int *right_halo = (int *)depv[2].ptr;

  expect_guid_mode("center owned write", depv[0], center_guid, DB_MODE_EW);
  expect_guid_mode("left halo read slice", depv[1], left_guid, DB_MODE_PTR);
  expect_guid_mode("right halo read slice", depv[2], right_guid, DB_MODE_PTR);
  expect_quad("center initial block", center, 100, 200, 300, 400);
  expect_pair("left halo values", left_halo, 30, 40);
  expect_pair("right halo values", right_halo, 50, 60);

  center[0] = left_halo[0] + right_halo[0];
  center[1] = left_halo[1] + right_halo[1];
  center[2] += left_halo[0];
  center[3] += right_halo[1];

  arts_printf("  PASS: center owner consumed RO halos and updated whole DB\n");
}

void check_center_slice_after_write(uint32_t paramc, const uint64_t *paramv,
                                    uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t expected_guid = (arts_guid_t)paramv[0];
  int *slice = (int *)depv[0].ptr;
  expect_guid_mode("center post-write slice", depv[0], expected_guid,
                   DB_MODE_PTR);
  expect_pair("center post-write slice payload", slice, 80, 100);
  arts_printf("  PASS: post-write RO slice observed owner updates\n");
}

void check_wavefront_result(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t left_guid = (arts_guid_t)paramv[0];
  arts_guid_t center_guid = (arts_guid_t)paramv[1];
  arts_guid_t right_guid = (arts_guid_t)paramv[2];
  int *left = (int *)depv[0].ptr;
  int *center = (int *)depv[1].ptr;
  int *right = (int *)depv[2].ptr;

  expect_guid_mode("left block result dep", depv[0], left_guid, DB_MODE_RO);
  expect_guid_mode("center block result dep", depv[1], center_guid, DB_MODE_RO);
  expect_guid_mode("right block result dep", depv[2], right_guid, DB_MODE_RO);
  expect_quad("left block final", left, 10, 20, 30, 40);
  expect_quad("center block final", center, 80, 100, 330, 460);
  expect_quad("right block final", right, 50, 60, 70, 80);
  arts_printf("  PASS: wavefront-style owner write preserved halo sources\n");
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== wavefront_dep_at ===\n");

  arts_guid_t epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);

  void *left_ptr = NULL;
  arts_guid_t left_db =
      arts_db_create(&left_ptr, 4 * sizeof(int), ARTS_DB_DEFAULT, NULL);
  int *left_data = (int *)left_ptr;
  left_data[0] = 10;
  left_data[1] = 20;
  left_data[2] = 30;
  left_data[3] = 40;
  arts_db_release(left_db);

  void *center_ptr = NULL;
  arts_guid_t center_db =
      arts_db_create(&center_ptr, 4 * sizeof(int), ARTS_DB_DEFAULT, NULL);
  int *center_data = (int *)center_ptr;
  center_data[0] = 100;
  center_data[1] = 200;
  center_data[2] = 300;
  center_data[3] = 400;
  arts_db_release(center_db);

  void *right_ptr = NULL;
  arts_guid_t right_db =
      arts_db_create(&right_ptr, 4 * sizeof(int), ARTS_DB_DEFAULT, NULL);
  int *right_data = (int *)right_ptr;
  right_data[0] = 50;
  right_data[1] = 60;
  right_data[2] = 70;
  right_data[3] = 80;
  arts_db_release(right_db);

  uint64_t left_slice_param[] = {(uint64_t)left_db};
  arts_guid_t leftSliceEdt = arts_edt_create_with_epoch(
      check_left_halo_slice, 1, left_slice_param, 1, epoch,
      &(arts_hint_t){.route = 0});
  arts_add_dependence_at(left_db, leftSliceEdt, 0, DB_MODE_RO,
                         2 * sizeof(int), 2 * sizeof(int));

  uint64_t wavefront_params[] = {(uint64_t)center_db, (uint64_t)left_db,
                                 (uint64_t)right_db};
  arts_guid_t centerStepEdt = arts_edt_create_with_epoch(
      apply_center_wavefront_step, 3, wavefront_params, 3, epoch,
      &(arts_hint_t){.route = 0});
  arts_add_dependence(center_db, centerStepEdt, 0, DB_MODE_EW);
  arts_add_dependence_at(left_db, centerStepEdt, 1, DB_MODE_RO,
                         2 * sizeof(int), 2 * sizeof(int));
  arts_add_dependence_at(right_db, centerStepEdt, 2, DB_MODE_RO, 0,
                         2 * sizeof(int));

  uint64_t center_slice_param[] = {(uint64_t)center_db};
  arts_guid_t centerSliceEdt = arts_edt_create_with_epoch(
      check_center_slice_after_write, 1, center_slice_param, 1, epoch,
      &(arts_hint_t){.route = 0});
  arts_add_dependence_at(center_db, centerSliceEdt, 0, DB_MODE_RO, 0,
                         2 * sizeof(int));

  uint64_t result_params[] = {(uint64_t)left_db, (uint64_t)center_db,
                              (uint64_t)right_db};
  arts_guid_t resultEdt = arts_edt_create_with_epoch(
      check_wavefront_result, 3, result_params, 3, epoch,
      &(arts_hint_t){.route = 0});
  arts_add_dependence(left_db, resultEdt, 0, DB_MODE_RO);
  arts_add_dependence(center_db, resultEdt, 1, DB_MODE_RO);
  arts_add_dependence(right_db, resultEdt, 2, DB_MODE_RO);

  arts_wait_on_handle(epoch);
  arts_shutdown();
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
