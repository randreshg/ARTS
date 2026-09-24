/* Standalone: no runtime, no fam module.  Asserts the configured pool base is
 * mappable and stays mapped while the process allocates and threads. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef ARTS_FAM_BASE
#error "ARTS_FAM_BASE not defined"
#endif

/* Larger than any pool a test cfg asks for, so a pass here covers them all. */
#define PROBE_BYTES ((size_t)1024 * 1024 * 1024)
#define PROBE_FILL 0x5A

static void *churn(void *arg) {
  void *p[64];
  for (int i = 0; i < 64; i++) {
    p[i] = malloc(1u << 16);
    if (p[i]) {
      memset(p[i], 0xAB, 1u << 16);
    }
  }
  for (int i = 0; i < 64; i++) {
    free(p[i]);
  }
  return arg;
}

int main(void) {
  void *want = (void *)(uintptr_t)ARTS_FAM_BASE;
  void *got = mmap(want, PROBE_BYTES, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE |
                       MAP_NORESERVE,
                   -1, 0);
  if (got == MAP_FAILED) {
    printf("FAIL fam_base_address: %p not mappable\n", want);
    return 1;
  }
  if (got != want) {
    munmap(got, PROBE_BYTES);
    printf("FAIL fam_base_address: %p taken, kernel offered %p\n", want, got);
    return 1;
  }
  memset(got, PROBE_FILL, 4096);
  memset((char *)got + PROBE_BYTES - 4096, PROBE_FILL, 4096);

  pthread_t t[8];
  for (int i = 0; i < 8; i++) {
    pthread_create(&t[i], NULL, churn, NULL);
  }
  for (int i = 0; i < 8; i++) {
    pthread_join(t[i], NULL);
  }
  void *big = malloc(256u << 20);
  if (big) {
    memset(big, 1, 256u << 20);
    free(big);
  }

  const unsigned char *c = (const unsigned char *)got;
  int intact = c[0] == PROBE_FILL && c[4095] == PROBE_FILL &&
               c[PROBE_BYTES - 1] == PROBE_FILL;
  munmap(got, PROBE_BYTES);
  if (!intact) {
    printf("FAIL fam_base_address: %p was clobbered after mapping\n", want);
    return 1;
  }
  printf("PASS fam_base_address\n");
  return 0;
}
