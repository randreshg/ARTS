# fft_hpx (fft_hpx_loop)

origin: third_party/hpx-fft/src/ (DaRUS doi:10.18419/darus-4520, "A HPX
  Communication Benchmark: Distributed FFT using Collectives", Strack and
  Pflüger, the dataset accompanying the Euro-Par 2024 poster paper)
pin: per-file SHA256, recorded in third_party/hpx-fft/PROVENANCE.md -- the
  dataset's download endpoint builds its archive on request, so the archive's
  own hash is not reproducible and the two files are pinned individually
licence: BSL-1.0
files:
- `src/fft_hpx_loop.cpp` -> `fft_hpx_loop.cpp`
- `src/vector_2d.hpp` -> `vector_2d.hpp`
edits:
- cfg: the run-everywhere cfg vector becomes the runtime defaults, which carry
  the same `hpx.run_hpx_main` line
- markers: `[HPX]` geometry from every locality once the locality count is
  known; `[E2E]` on locality 0 from the first statement of `hpx_main` to
  immediately before its `hpx::finalize()` -- the whole application, runtime
  start-up and teardown excluded, the span every runtime of the comparison
  reports (every locality runs `hpx_main`; the checksum reduction every
  locality takes part in precedes locality 0's end); `[PARCELS]` after that
  when the structural marker is set. The program's own
  `high_resolution_timer` stays exactly where it was and keeps timing what it
  timed
- scalar: the program computes no printable result of its own (it prints a
  phase-by-phase timing table), so the edit is the simplest digest of the
  final state it already holds: each locality sums its own rows and one
  `all_reduce` over a communicator of its own makes the total, which locality
  0 writes once as `CHECKSUM %.14g`. It sits before the end stamp, inside the
  span, as the mirror's own tally does
- bugfix: `basenames_` held `const char*` and was filled with
  `std::move(std::to_string(i).c_str())` -- a pointer into a temporary that
  dies at the end of that statement, read by `create_communicator` in the
  next one. The read is undefined behaviour whether or not a given toolchain
  leaves the bytes intact, and it is what names every one of the program's
  `num_localities` scatter communicators. The names are held in a
  `std::vector<std::string>` instead and the two call sites pass `.c_str()`,
  which is local, changes no structure and no communicator, and leaves the
  collective count and the algorithm untouched. Recorded as evidence rather
  than as the trigger: the unmodified copy built here ran to completion and
  exited 0 at one, two, four and eight localities before the fix was made
