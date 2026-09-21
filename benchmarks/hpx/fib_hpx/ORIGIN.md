# fib_hpx (fibonacci_futures_distributed)

origin: third_party/hpx/examples/quickstart/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Hartmut Kaiser, 2007-2013)
files:
- `examples/quickstart/fibonacci_futures_distributed.cpp` -> `fibonacci_futures_distributed.cpp`
edits:
- markers: `[HPX]` geometry from every locality (startup function; hpx_main runs on locality 0 only), `[E2E]` on locality 0 from the first statement of `hpx_main` to immediately before its `hpx::finalize()` -- the whole application, runtime start-up and teardown excluded, the span every runtime of the comparison reports, `[PARCELS]` after the end stamp when the structural marker is set
- cfg: the runtime defaults without `run_hpx_main` (the program's hpx_main is locality 0's driver by design)
- kernel: `fibonacci_serial_sub`'s definition moves, its statements unchanged, to `benchmarks/apps/hpx_origin/fib_serial_kernel.c`, and the program declares it `extern "C"`.  At the row's arguments a run is about `1.3·10¹³` calls of this function against about 21 900 tasks, so the span is the function; compiled once and linked by this program and by every OCR entry, it is the same machine code on every side -- and starts on a 64-byte boundary in every program, since where a linker happens to place a function this small and this hot moves it by as much again -- where two compilations of the same four lines -- one as C++ inside this translation unit, one as C -- differ by up to a tenth in either direction.  `fibonacci_serial`, its counter and every call site are untouched
