# stencil1d_hpx (1d_stencil_8)

origin: third_party/hpx/examples/1d_stencil/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Hartmut Kaiser, 2014-2022)
files:
- `examples/1d_stencil/1d_stencil_8.cpp` -> `1d_stencil_8.cpp`
- `examples/1d_stencil/print_time_results.hpp` -> `print_time_results.hpp`
edits:
- build: the program registers its own component module, whose factory and registry plugin lists its two component registrations refer to (the build gives each program a component name of its own, so the runtime's default module is not the one they name)
- cfg: the run-everywhere cfg vector becomes the runtime defaults, which carry the same `hpx.run_hpx_main` line
- markers: `[HPX]` geometry from every locality at the top of `do_all_work`, `[E2E]` on locality 0 from the first statement of `hpx_main` to immediately before its `hpx::finalize()` -- the whole application, runtime start-up and teardown excluded, the span every runtime of the comparison reports (every locality runs `hpx_main`; locality 0 gathers every other's partitions before it returns from `do_all_work`, so its end is the application's), `[PARCELS]` after the end stamp when the structural marker is set
- scalar: the gather loop keeps the partition data it already pulls and prints `CHECKSUM %.14g`, the sum of every final partition element, by one write
