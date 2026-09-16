/* The vendored dependencies resolve as targets: one entry point from each,
 * linked the way an application links them. */
#include <fftw3.h>
#include <hdf5.h>

int main(void) {
  double in[4] = {0, 1, 2, 3};
  fftw_complex *out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * 4);
  fftw_plan p = fftw_plan_dft_r2c_1d(4, in, out, FFTW_ESTIMATE);
  fftw_execute(p);
  fftw_destroy_plan(p);
  fftw_free(out);
  return H5open() < 0 ? 1 : (H5close() < 0 ? 1 : 0);
}
