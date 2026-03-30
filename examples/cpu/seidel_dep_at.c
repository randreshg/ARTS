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

/// @file seidel_dep_at.c
/// @brief Minimal Seidel-style example using whole-DB writes plus RO halo
///        slices from arts_add_dependence_at.

#include "arts.h"
#include <stdlib.h>

static void fail_example(const char *msg) {
  arts_printf("seidel_dep_at FAIL: %s\n", msg);
  abort();
}

static void print_block(const char *label, const int *data) {
  arts_printf("%s [%d, %d, %d, %d]\n", label, data[0], data[1], data[2],
              data[3]);
}

void apply_seidel_step(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;

  int *center = (int *)depv[0].ptr;
  int *left_halo = (int *)depv[1].ptr;
  int *right_halo = (int *)depv[2].ptr;

  if (!center || !left_halo || !right_halo)
    fail_example("missing dependency payload");
  if (depv[0].mode != DB_MODE_EW)
    fail_example("center block must arrive as DB_MODE_EW");
  if (depv[1].mode != DB_MODE_PTR || depv[2].mode != DB_MODE_PTR)
    fail_example("halo slices must arrive as DB_MODE_PTR");

  arts_printf("apply_seidel_step: center whole-DB write + two RO halos\n");
  print_block("  center before", center);
  arts_printf("  left halo [%d, %d]\n", left_halo[0], left_halo[1]);
  arts_printf("  right halo [%d, %d]\n", right_halo[0], right_halo[1]);

  center[0] = left_halo[0] + right_halo[0];
  center[1] = left_halo[1] + right_halo[1];
  center[2] += left_halo[0];
  center[3] += right_halo[1];
}

void finish_example(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;

  int *left = (int *)depv[0].ptr;
  int *center = (int *)depv[1].ptr;
  int *right = (int *)depv[2].ptr;

  if (!left || !center || !right)
    fail_example("missing final blocks");

  arts_printf("seidel_dep_at final state\n");
  print_block("  left ", left);
  print_block("  center", center);
  print_block("  right", right);

  if (center[0] != 80 || center[1] != 100 || center[2] != 330 ||
      center[3] != 460)
    fail_example("unexpected final center block");
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_guid_t epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);

  void *left_ptr = NULL;
  arts_guid_t left_db =
      arts_db_create(&left_ptr, 4 * sizeof(int), ARTS_DB_DEFAULT, NULL);
  int *left = (int *)left_ptr;
  left[0] = 10;
  left[1] = 20;
  left[2] = 30;
  left[3] = 40;
  arts_db_release(left_db);

  void *center_ptr = NULL;
  arts_guid_t center_db =
      arts_db_create(&center_ptr, 4 * sizeof(int), ARTS_DB_DEFAULT, NULL);
  int *center = (int *)center_ptr;
  center[0] = 100;
  center[1] = 200;
  center[2] = 300;
  center[3] = 400;
  arts_db_release(center_db);

  void *right_ptr = NULL;
  arts_guid_t right_db =
      arts_db_create(&right_ptr, 4 * sizeof(int), ARTS_DB_DEFAULT, NULL);
  int *right = (int *)right_ptr;
  right[0] = 50;
  right[1] = 60;
  right[2] = 70;
  right[3] = 80;
  arts_db_release(right_db);

  arts_guid_t step = arts_edt_create_with_epoch(apply_seidel_step, 0, NULL, 3,
                                                epoch,
                                                &(arts_hint_t){.route = 0});
  arts_add_dependence(center_db, step, 0, DB_MODE_EW);
  arts_add_dependence_at(left_db, step, 1, DB_MODE_RO, 2 * sizeof(int),
                         2 * sizeof(int));
  arts_add_dependence_at(right_db, step, 2, DB_MODE_RO, 0, 2 * sizeof(int));

  arts_guid_t finish = arts_edt_create_with_epoch(finish_example, 0, NULL, 3,
                                                  epoch,
                                                  &(arts_hint_t){.route = 0});
  arts_add_dependence(left_db, finish, 0, DB_MODE_RO);
  arts_add_dependence(center_db, finish, 1, DB_MODE_RO);
  arts_add_dependence(right_db, finish, 2, DB_MODE_RO);

  arts_wait_on_handle(epoch);
  arts_shutdown();
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
