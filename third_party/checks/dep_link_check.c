/* The vendored dependency resolves as a target: one entry point, linked the
 * way an application links it. */
#include <fftw3.h>

int main(void) {
  double in[4] = {0, 1, 2, 3};
  fftw_complex *out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * 4);
  fftw_plan p = fftw_plan_dft_r2c_1d(4, in, out, FFTW_ESTIMATE);
  fftw_execute(p);
  fftw_destroy_plan(p);
  fftw_free(out);
  return 0;
}
