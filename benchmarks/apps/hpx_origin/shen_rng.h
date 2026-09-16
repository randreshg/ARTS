#ifndef SHEN_RNG_H
#define SHEN_RNG_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int32_t words[31];
  unsigned front, rear;
  atomic_flag lock;
} shen_rng_t;

static inline void shen_rng_init(shen_rng_t *s) {
  /* The compiled default stream has already undergone seed-1 warmup. */
  static const int32_t initial[31] = {
      -1726662223, 379960547, 1735697613, 1040273694, 1313901226,
      1627687941, -179304937, -2073333483, 1780058412, -1989503057,
      -615974602, 344556628, 939512070, -1249116260, 1507946756,
      -812545463, 154635395, 1388815473, -1926676823, 525320961,
      -1009028674, 968117788, -123449607, 1284210865, 435012392,
      -2017506339, -911064859, -370259173, 1132637927, 1398500161,
      -205601318};
  memcpy(s->words, initial, sizeof initial);
  s->front = 3;
  s->rear = 0;
  atomic_flag_clear(&s->lock);
}

static inline void shen_rng_lock(shen_rng_t *s) {
  while (atomic_flag_test_and_set_explicit(&s->lock, memory_order_acquire)) {}
}

/* Pointers exist only inside the current acquisition and locked call. */
static inline struct random_data shen_rng_view(shen_rng_t *s) {
  return (struct random_data){
      .fptr = s->words + s->front, .rptr = s->words + s->rear,
      .state = s->words, .rand_type = 3, .rand_deg = 31, .rand_sep = 3,
      .end_ptr = s->words + 31};
}

static inline void shen_rng_save(shen_rng_t *s, const struct random_data *v) {
  s->front = (unsigned)(v->fptr - s->words);
  s->rear = (unsigned)(v->rptr - s->words);
  atomic_flag_clear_explicit(&s->lock, memory_order_release);
}

static inline void shen_rng_seed(shen_rng_t *s, unsigned seed) {
  shen_rng_lock(s);
  struct random_data v = shen_rng_view(s);
  if (srandom_r(seed, &v) != 0) abort();
  shen_rng_save(s, &v);
}

static inline int32_t shen_rng_draw(shen_rng_t *s) {
  int32_t result;
  shen_rng_lock(s);
  struct random_data v = shen_rng_view(s);
  if (random_r(&v, &result) != 0) abort();
  shen_rng_save(s, &v);
  return result;
}

#endif
