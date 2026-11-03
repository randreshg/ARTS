/* SPDX-License-Identifier: Apache-2.0
 *
 * Numeric configuration values are read as written or refused by name.
 *
 * Properties:
 *   - an unsigned-int key accepts decimal digits (leading zeros included, up
 *     to the type's maximum) and refuses a sign, a space, trailing text, a
 *     non-decimal base, an empty value and anything past the maximum;
 *   - a 64-bit key does the same at the 64-bit maximum;
 *   - a boolean key accepts exactly 0 and 1;
 *   - node_count and the bounds of a host range follow the unsigned-int rule;
 *   - a refused value leaves no field changed.
 *
 * The parse helpers are file-static, so config.c is included whole;
 * ARTS_ERROR reaches the longjmp abort stub of config_test_common.h.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr, "FAIL config_value_syntax: " __VA_ARGS__);         \
      (void)fputc('\n', stderr);                                               \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static const struct arts_config_entry_s *entry_of(const char *key) {
  for (const struct arts_config_entry_s *e = config_entries; e->key; e++) {
    if (strcmp(e->key, key) == 0) {
      return e;
    }
  }
  return NULL;
}

/* Parse `value` for `key` into a zeroed config: 1 when it was refused. */
static int key_refused(const char *key, const char *value,
                       struct arts_config_s *out) {
  const struct arts_config_entry_s *e = entry_of(key);
  CHECK(e != NULL, "no config key %s", key);
  memset(out, 0, sizeof(*out));
  g_abort_fired = 0;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    config_auto_parse(out, e, value);
    g_abort_armed = 0;
    return 0;
  }
  return 1;
}

static uint64_t field_of(const char *key, const struct arts_config_s *c) {
  const struct arts_config_entry_s *e = entry_of(key);
  const char *base = (const char *)c;
  switch (e->type) {
  case CONFIG_UINT:
    return *(const unsigned int *)(base + e->offset);
  case CONFIG_UINT64:
    return *(const uint64_t *)(base + e->offset);
  default:
    return *(const bool *)(base + e->offset);
  }
}

static void accepts(const char *key, const char *value, uint64_t want) {
  struct arts_config_s c;
  if (key_refused(key, value, &c)) {
    CHECK(0, "%s='%s' was refused", key, value);
    return;
  }
  CHECK(field_of(key, &c) == want, "%s='%s' read as %llu, want %llu", key,
        value, (unsigned long long)field_of(key, &c), (unsigned long long)want);
}

static void refuses(const char *key, const char *value) {
  struct arts_config_s c;
  int refused = key_refused(key, value, &c);
  CHECK(refused == 1, "%s='%s' was accepted", key, value);
  if (refused) {
    CHECK(field_of(key, &c) == 0, "%s='%s' changed its field", key, value);
  }
}

static int node_count_refused(const char *value, unsigned int *out) {
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    *out = config_node_count_value(value);
    g_abort_armed = 0;
    return 0;
  }
  return 1;
}

/* A host list on the Slurm (master_boot false) or ssh (true) table path:
 * 1 when a range bound was refused. */
static int hosts_refused(bool master_boot, const char *list) {
  char buf[128];
  (void)snprintf(buf, sizeof(buf), "%s", list);
  struct arts_config_s cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.master_boot = master_boot;
  cfg.nodes = 8;
  struct arts_config_s *p = &cfg;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    arts_config_create_routing_table(&p, buf);
    g_abort_armed = 0;
    arts_config_destroy(&cfg);
    return 0;
  }
  return 1;
}

int main(void) {
  static const char *const bad_numbers[] = {
      "3x", "-1", "+1", " 1", "1 ", "0x10", "1.5", "", "x", "1e3", "--1"};
  const size_t nbad = sizeof(bad_numbers) / sizeof(bad_numbers[0]);

  /* unsigned int */
  accepts("worker_threads", "3", 3);
  accepts("counter_capture_interval", "0", 0);
  accepts("counter_capture_interval", "007", 7);
  accepts("counter_capture_interval", "0000000000000000000000000012", 12);
  accepts("route_table_size", "4294967295", UINT_MAX);
  refuses("route_table_size", "4294967296");
  refuses("route_table_size", "18446744073709551616");
  refuses("route_table_size", "99999999999999999999999999");
  for (size_t i = 0; i < nbad; i++) {
    refuses("worker_threads", bad_numbers[i]);
    refuses("route_table_size", bad_numbers[i]);
  }

  /* uint64 */
  accepts("stack_size", "0", 0);
  accepts("stack_size", "4294967296", 4294967296ull);
  accepts("stack_size", "18446744073709551615", UINT64_MAX);
  refuses("stack_size", "18446744073709551616");
  refuses("stack_size", "184467440737095516150");
  for (size_t i = 0; i < nbad; i++) {
    refuses("stack_size", bad_numbers[i]);
  }

  /* bool */
  accepts("pin", "0", 0);
  accepts("pin", "1", 1);
  accepts("core_dump", "1", 1);
  accepts("core_dump", "0", 0);
  accepts("core_dump", "00", 0);
  accepts("core_dump", "01", 1);
  refuses("pin", "2");
  refuses("pin", "10");
  refuses("pin", "true");
  refuses("pin", "false");
  refuses("core_dump", "9999999999");
  for (size_t i = 0; i < nbad; i++) {
    refuses("pin", bad_numbers[i]);
  }

  /* node_count */
  unsigned int n = 0;
  CHECK(node_count_refused("4", &n) == 0 && n == 4, "node_count=4");
  CHECK(node_count_refused("0004", &n) == 0 && n == 4, "node_count=0004");
  CHECK(node_count_refused("0", &n) == 0 && n == 0, "node_count=0 stays for "
                                                    "the launcher's check");
  CHECK(node_count_refused("4294967295", &n) == 0 && n == UINT_MAX,
        "node_count=UINT_MAX");
  CHECK(node_count_refused("4294967296", &n) == 1, "node_count past UINT_MAX");
  for (size_t i = 0; i < nbad; i++) {
    CHECK(node_count_refused(bad_numbers[i], &n) == 1, "node_count='%s'",
          bad_numbers[i]);
  }

  /* host-range bounds, on both table paths */
  for (int boot = 0; boot < 2; boot++) {
    bool mb = boot != 0;
    CHECK(hosts_refused(mb, "j[1-3]") == 0, "range j[1-3] (%d)", boot);
    CHECK(hosts_refused(mb, "j[01-03]") == 0, "range j[01-03] (%d)", boot);
    CHECK(hosts_refused(mb, "j[3-1]") == 0, "range j[3-1] (%d)", boot);
    CHECK(hosts_refused(mb, "j[1x-3]") == 1, "range j[1x-3] (%d)", boot);
    CHECK(hosts_refused(mb, "j[1-3x]") == 1, "range j[1-3x] (%d)", boot);
    CHECK(hosts_refused(mb, "j[+1-3]") == 1, "range j[+1-3] (%d)", boot);
    CHECK(hosts_refused(mb, "j[1-99999999999]") == 1,
          "range j[1-99999999999] (%d)", boot);
    CHECK(hosts_refused(mb, "j[4294967296-1]") == 1,
          "range j[4294967296-1] (%d)", boot);
  }

  if (fails != 0) {
    return 1;
  }
  printf("PASS config_value_syntax: unsigned int, 64-bit, bool, node_count "
         "and host-range values read as written or refused\n");
  return 0;
}
