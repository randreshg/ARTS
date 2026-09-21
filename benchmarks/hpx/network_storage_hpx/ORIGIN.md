# network_storage_hpx (network_storage)

origin: third_party/hpx/tests/performance/network/network_storage/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (John Biddiscombe, 2014-2015)
files:
- `tests/performance/network/network_storage/network_storage.cpp` -> `network_storage.cpp`
- `tests/performance/network/network_storage/simple_profiler.hpp` -> `simple_profiler.hpp`
edits:
- cfg: the run-everywhere cfg vector becomes the runtime defaults, which carry the same `hpx.run_hpx_main` line
- markers: `[HPX]` geometry from every locality through a startup function, which every locality runs before `hpx_main` starts on any of them, so each geometry line is written before the test's own output, which writes its lines in pieces that a launcher merging the streams could otherwise split a marker line into; `[E2E]` on locality 0 from the first statement of `hpx_main` to immediately before its `hpx::finalize()` -- the whole application, runtime start-up and teardown excluded, the span every runtime of the comparison reports (only locality 0 finalizes, after the tally every locality takes part in), `[PARCELS]` after the end stamp when the structural marker is set.  The window holds the storage allocation, the warm-up write, the write and the read with the barriers that open and close each, the tally and the release of the storage; the origin's own `high_resolution_timer`s time each test separately -- the warm-up's time is printed too, only its CSV record and its profile table are suppressed -- and stay exactly where they are
- scalar: two per-locality atomics count the transfers completed by the continuations that already receive each transfer's result — the write continuation that passes on `CopyToStorage`'s `TEST_SUCCESS`, and the read continuation that returns `TEST_SUCCESS` once the bytes are copied — and before the end stamp an `all_reduce` sums them over the localities; locality 0 prints `TRANSFERS_OK <writes> <reads>` by one write.  The warm-up pass runs through the same write continuation, so it is counted
- bugfix: the pointer allocator's `deallocate` checks `HPX_TEST_EQ(p == pointer_ && n, size_)`, which hands the macro the `bool` `p == pointer_ && n` and the transfer size as its two operands, so it compares `1` with a size in bytes that is a multiple of 1024 and can never pass; it printed a failed check for every read that crossed a locality.  The line is commented out

The test's own `CMakeLists.txt`, launch-script templates and plotting scripts
are not sources and are not carried; the app project builds the one
translation unit, and names the runtime's iostreams component, which the
test's build file declares as a dependency, in its link list.  The program
defines plain actions and no component, so it needs no component module of its
own.  The storage's unsynchronised copies are the test's design and are left
exactly as written.
