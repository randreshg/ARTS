# hpx-fft (pristine origin)

Source: DaRUS doi:10.18419/darus-4520, "A HPX Communication Benchmark:
Distributed FFT using Collectives" (Strack & Pflüger), the dataset that
accompanies the Euro-Par 2024 poster paper.  Licence: BSL-1.0 (LICENSE).

Only the two files an HPX-origin program is built from are kept; the
dataset's installation and benchmark scripts are not used.  The dataset's
download endpoint builds its archive on request, so the archive's hash is not
reproducible and the files are pinned instead:

    41deb3bbfe513d1bab025dca2c6314cb8a22a8eeef37ee1f176c71e309a32e87  src/fft_hpx_loop.cpp
    4f4198ac532e63a519e0c43264e14a37c02dce67814bef429279992992d05417  src/vector_2d.hpp

This tree is never edited.  The edited copy is benchmarks/hpx/fft_hpx/, and
the whole difference between the two is that program's origin.patch.
