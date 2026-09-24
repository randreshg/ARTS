/* arts_fam_contains must be callable, and answer false, before any init --
 * a caller on a teardown or early-boot path has no way to know. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "arts/fam/pool.h"

void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes) {
  (void)rank;
  (void)nranks;
  *out_base = NULL;
  *out_bytes = 0;
}
void arts_fam_backend_unmap(void *base, uint64_t bytes) {
  (void)base;
  (void)bytes;
}
void arts_fam_backend_flush(const void *p, size_t bytes, bool producer) {
  (void)p;
  (void)bytes;
  (void)producer;
}
void arts_fam_backend_config_check(const struct arts_config_s *c) { (void)c; }

int main(void) {
  int automatic = 0;
  void *heap = malloc(4096);
  if (arts_fam_contains(NULL) || arts_fam_contains(&automatic) ||
      arts_fam_contains(heap)) {
    printf("FAIL fam_contains_preinit\n");
    return 1;
  }
  free(heap);
  printf("PASS fam_contains_preinit\n");
  return 0;
}
