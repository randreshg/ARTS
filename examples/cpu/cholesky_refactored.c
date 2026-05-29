// OpenMP-parallelized Cholesky factorization (DPOTRF): A = L * L^T
//
// A is an N×N symmetric positive-definite matrix stored in column-major order.
// The sub-diagonal row updates within each column are independent and are
// parallelized with #pragma omp parallel for.
//
// Build:  g++ -O3 -march=native -fopenmp -o cholesky_openmp cholesky_openmp.cpp
// -lm Run:    OMP_NUM_THREADS=8 ./cholesky_openmp [N] [repeats]

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arts.h"

int N;
int repeats;
double *A;
double *L;

// Column-major access macros
#define A(i, j) A[(size_t)(j) * N + (i)]
#define L(i, j) L[(size_t)(j) * N + (i)]

// ---------------------------------------------------------------------------
// OpenMP Cholesky — parallelize the sub-diagonal row updates in each column.
// Rows i > j are fully independent once L(j,j) is known, making this a
// natural fork/join over the trailing column panel.
// ---------------------------------------------------------------------------
void cholesky_openmp(const double *A, double *L, int N) {
  memset(L, 0, (size_t)N * N * sizeof(double));
  for (int j = 0; j < N; j++) {
    // Diagonal element (sequential — depends on previous columns)
    double sum = A(j, j);
    for (int k = 0; k < j; k++)
      sum -= L(j, k) * L(j, k);
    L(j, j) = sqrt(sum);

    double inv_ljj = 1.0 / L(j, j);

// Sub-diagonal elements — rows are independent, parallelize over i
#pragma omp parallel for schedule(static)
    for (int i = j + 1; i < N; i++) {
      double s = A(i, j);
      for (int k = 0; k < j; k++)
        s -= L(i, k) * L(j, k);
      L(i, j) = s * inv_ljj;
    }
  }
}

// ---------------------------------------------------------------------------
// Generate a random symmetric positive-definite matrix:
//   A = M^T * M + N*I
// ---------------------------------------------------------------------------
void generate_spd_matrix(double *A, int N) {
  double *M = (double *)malloc((size_t)N * N * sizeof(double));
  for (size_t i = 0; i < (size_t)N * N; i++)
    M[i] = (double)rand() / RAND_MAX;

  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      double s = 0.0;
      for (int k = 0; k < N; k++)
        s += M[(size_t)k * N + i] * M[(size_t)k * N + j];
      A(i, j) = s;
    }

  for (int i = 0; i < N; i++)
    A(i, i) += N;

  free(M);
}

// ---------------------------------------------------------------------------
// Frobenius norm of (A - L*L^T) over the lower triangle.
// ---------------------------------------------------------------------------
double verify(const double *A, const double *L, int N) {
  double err = 0.0;
  for (int i = 0; i < N; i++)
    for (int j = 0; j <= i; j++) {
      double s = 0.0;
      for (int k = 0; k <= j; k++)
        s += L(i, k) * L(j, k);
      double diff = A(i, j) - s;
      err += diff * diff;
    }
  return sqrt(err);
}

int main(int argc, char **argv) {
  int N = 1024;
  int repeats = 3;
  if (argc > 1)
    N = atoi(argv[1]);
  if (argc > 2)
    repeats = atoi(argv[2]);

  int num_threads = 0;
#pragma omp parallel
  {
#pragma omp single
    num_threads = omp_get_num_threads();
  }

  printf("OpenMP Cholesky factorization: N = %d, threads = %d, repeats = %d\n",
         N, num_threads, repeats);

  double *A = (double *)malloc((size_t)N * N);
  double *L = (double *)malloc((size_t)N * N);

  srand(42);
  generate_spd_matrix(A, N);

  // Warm-up
  cholesky_openmp(A, L, N);

  // Timed runs — keep best wall-clock time
  double best_time = 1e30;
  double total_time = 0.0;
  struct timespec t0, t1;
  for (int r = 0; r < repeats; r++) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cholesky_openmp(A, L, N);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ns =
        (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    double elapsed = elapsed_ns / 1e9;
    total_time += elapsed;
    if (elapsed < best_time)
      best_time = elapsed;
    printf("  run %d: %.6f s\n", r + 1, elapsed);
  }

  double residual = verify(A, L, N);
  double gflops = (1.0 / 3.0) * (double)N * N * N / best_time / 1e9;

  printf("  Best time:  %.6f s\n", best_time);
  printf("  Avg time:   %.6f s\n", total_time / repeats);
  printf("  GFLOP/s:    %.3f\n", gflops);
  printf("  Residual:   %.2e\n", residual);

  free(A);
  free(L);
  return 0;
}

void cholesky_kernel(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {}

void run_cholesky(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  arts_printf("Running Cholesky\n");
}

void init_per_node(unsigned int node_id, int argc, char **argv) {
#if ARTS_USE_CXL
  printf("Using CXL\n");
#else
  printf("Using default DBs\n");
#endif
  if (!node_id) {
    N = 1024;
    repeats = 3;
    if (argc > 1)
      N = atoi(argv[1]);
    if (argc > 2)
      repeats = atoi(argv[2]);

    printf("ARTS Cholesky factorization: N = %d, workers = %d, repeats = %d\n",
           N, arts_get_total_workers() * arts_get_total_nodes(), repeats);

    A = (double *)malloc((size_t)N * N);
    L = (double *)malloc((size_t)N * N);

    srand(42);
    generate_spd_matrix(A, N);

    arts_edt_create(run_cholesky, 0, NULL, 0, &(arts_hint_t){.route = 0})
  }
}

void init_per_worker(unsigned int node_id, unsigned int worker_id, int argc,
                     char **argv) {}

int main(int argc, char **argv) {
  arts_printf("Starting ArtsRT with Cholesky\n");
  arts_rt(argc, argv);
  return 0;
}
