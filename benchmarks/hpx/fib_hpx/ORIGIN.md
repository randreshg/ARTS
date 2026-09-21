# fib_hpx (fibonacci_futures_distributed)

origin: third_party/hpx/examples/quickstart/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Hartmut Kaiser, 2007-2013)
files:
- `examples/quickstart/fibonacci_futures_distributed.cpp` -> `fibonacci_futures_distributed.cpp`
edits:
- markers: `[HPX]` geometry from every locality (startup function; hpx_main runs on locality 0 only), `[E2E]` on locality 0 from the first statement of `hpx_main` to immediately before its `hpx::finalize()` -- the whole application, runtime start-up and teardown excluded, the span every runtime of the comparison reports, `[PARCELS]` after the end stamp when the structural marker is set
- cfg: the runtime defaults without `run_hpx_main` (the program's hpx_main is locality 0's driver by design)
