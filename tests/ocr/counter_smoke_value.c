/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/

/// @file counter_smoke_value.c
/// @brief A counter VALUE in n0.json, end to end: the increment path
///        (arts_counter_increment_by), node reduction and the JSON write.
///
/// The per-node JSON (n0.json) carries a "NUM_EDT_CREATE" and a
/// "NUM_EDT_FINISH" object whose numeric "value" is reduced (SUM) across this
/// node's threads.  We create a known number N of EDTs; with the main EDT the
/// runtime creates and runs, each value must be exactly N + 1.  The test is
/// registered only in a tree whose counter config captures both counters.

#include "arts.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_EDTS 64

void worker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (int i = 0; i < N_EDTS; i++) {
    arts_edt_create(worker_edt, 0, NULL, 0,
                    &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
  }
  arts_event_wait(fe);
  arts_shutdown();
}

/* Locate counter object `name` inside the JSON, then read its "value" field.
   Returns true and sets *out on success; false if the counter or value is
   missing. */
static bool read_counter_value(const char *json, const char *name,
                               uint64_t *out) {
  char key[128];
  (void)snprintf(key, sizeof(key), "\"%s\"", name);
  const char *obj = strstr(json, key);
  if (!obj) {
    return false;
  }
  /* The "value" key inside this counter's object is the first "value" after
     the counter name.  Counter objects are small and self-contained, so the
     nearest "value" belongs to this counter. */
  const char *v = strstr(obj, "\"value\"");
  if (!v) {
    return false;
  }
  v = strchr(v + strlen("\"value\""), ':');
  if (!v) {
    return false;
  }
  v++;
  while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') {
    v++;
  }
  char *end = NULL;
  uint64_t val = strtoull(v, &end, 10);
  if (end == v) {
    return false;
  }
  *out = val;
  return true;
}

int main(int argc, char **argv) {
  /* A file an earlier run left must not stand in for this run's. */
  (void)remove("./counters/n0.json");
  if (arts_rt(argc, argv) != 0) {
    printf("FAIL counter_smoke_value: the runtime run failed\n");
    return 1;
  }

  FILE *fp = fopen("./counters/n0.json", "r");
  if (!fp) {
    printf("FAIL counter_smoke_value: ./counters/n0.json not found\n");
    return 1;
  }
  (void)fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  (void)fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    (void)fclose(fp);
    printf("FAIL counter_smoke_value: n0.json empty\n");
    return 1;
  }
  char *buf = (char *)malloc((size_t)sz + 1);
  size_t got = fread(buf, 1, (size_t)sz, fp);
  (void)fclose(fp);
  buf[got] = '\0';

  static const char *const names[] = {"NUM_EDT_CREATE", "NUM_EDT_FINISH"};
  const uint64_t want = (uint64_t)N_EDTS + 1u;
  int failures = 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    uint64_t value = 0;
    if (!read_counter_value(buf, names[i], &value)) {
      printf("FAIL counter_smoke_value: %s not present in n0.json\n", names[i]);
      failures++;
    } else if (value != want) {
      printf("FAIL counter_smoke_value: %s value %llu, expected %llu\n",
             names[i], (unsigned long long)value, (unsigned long long)want);
      failures++;
    }
  }
  free(buf);
  if (failures != 0) {
    return 1;
  }
  printf("PASS counter_smoke_value: NUM_EDT_CREATE = NUM_EDT_FINISH = %llu\n",
         (unsigned long long)want);
  return 0;
}
