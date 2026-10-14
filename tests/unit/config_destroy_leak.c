/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_config_destroy leak audit (LSan).
 *
 * Drives a full arts_config_load() end-to-end over a cfg the test writes and
 * names through ARTS_CONFIG.  The cfg populates: launcher (ssh), routing
 * table (3 hosts, each row with its own copy of the port list), ports,
 * master_node, net_interface, counter_folder, and a launcher_data via
 * arts_launcher_create.  Then arts_config_destroy frees them.
 *
 * Property (LSan): a single load+destroy leaks ZERO bytes.  launcher_data is
 * the one block arts_launcher_create allocated (the child-pid array a launch
 * allocates is freed by the launcher's own cleanup step), and destroy releases
 * a configuration once.
 *
 * Built with -fsanitize=address (LSan is part of ASan) to assert no leaks.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  /* ssh launcher, 3 hosts via bracket range, plus the string/array fields
     config_destroy must free. */
  static const char cfg_text[] = "launcher=ssh\n"
                                 "nodes=n[01-03]\n"
                                 "ports=[25000-25001]\n"
                                 "node_count=3\n"
                                 "net_interface=eth0\n"
                                 "counter_folder=./artscnt\n"
                                 "master_node=n01\n"
                                 "worker_threads=4\n";
  char path[] = "config_destroy_leak.XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    perror("FAIL destroy_leak: mkstemp");
    return 1;
  }
  FILE *out = fdopen(fd, "w");
  if (out == NULL) {
    close(fd);
    unlink(path);
    fprintf(stderr, "FAIL destroy_leak: cannot open %s\n", path);
    return 1;
  }
  int written = fputs(cfg_text, out) != EOF;
  if (fclose(out) != 0 || !written) {
    unlink(path);
    fprintf(stderr, "FAIL destroy_leak: cannot write %s\n", path);
    return 1;
  }
  setenv("ARTS_CONFIG", path, 1);

  /* An abort in the load lands here, so the cfg is removed on that path too. */
  struct arts_config_s cfg;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) != 0) {
    unlink(path);
    fprintf(stderr, "FAIL destroy_leak: arts_config_load aborted\n");
    return 1;
  }
  arts_config_load(&cfg);
  g_abort_armed = 0;
  unlink(path);

  /* Sanity: the fields config_destroy will free are actually populated. */
  if (cfg.launcher == NULL || cfg.table == NULL || cfg.table_length != 3) {
    fprintf(stderr,
            "FAIL destroy_leak: load did not populate (launcher=%p "
            "table=%p len=%u)\n",
            (void *)cfg.launcher, (void *)cfg.table, cfg.table_length);
    return 1;
  }
  if (cfg.master_node == NULL || cfg.net_interface == NULL ||
      cfg.counter_folder == NULL) {
    fprintf(stderr, "FAIL destroy_leak: string fields not populated\n");
    return 1;
  }

  /* Single destroy must free EVERYTHING (LSan asserts zero leaks at exit). */
  arts_config_destroy(&cfg);

  printf("PASS config_destroy_leak: full load+destroy leak-free\n");
  return 0;
}
