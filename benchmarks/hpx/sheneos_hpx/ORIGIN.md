# sheneos_hpx (sheneos_test)

origin: third_party/hpx/examples/sheneos/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Hartmut Kaiser, 2007-2024)
files:
- `examples/sheneos/sheneos_test.cpp` -> `sheneos_test.cpp`
- `examples/sheneos/sheneos/interpolator.hpp` -> `sheneos/interpolator.hpp`
- `examples/sheneos/sheneos/interpolator.cpp` -> `sheneos/interpolator.cpp`
- `examples/sheneos/sheneos/partition3d.hpp` -> `sheneos/partition3d.hpp`
- `examples/sheneos/sheneos/read_values.hpp` -> `sheneos/read_values.hpp`
- `examples/sheneos/sheneos/read_values.cpp` -> `sheneos/read_values.cpp`
- `examples/sheneos/sheneos/dimension.hpp` -> `sheneos/dimension.hpp`
- `examples/sheneos/sheneos/dimension.cpp` -> `sheneos/dimension.cpp`
- `examples/sheneos/sheneos/server/partition3d.hpp` -> `sheneos/server/partition3d.hpp`
- `examples/sheneos/sheneos/server/partition3d.cpp` -> `sheneos/server/partition3d.cpp`
- `examples/sheneos/sheneos/server/configuration.hpp` -> `sheneos/server/configuration.hpp`
- `examples/sheneos/sheneos/server/configuration.cpp` -> `sheneos/server/configuration.cpp`
edits:
- cfg: the runtime defaults are installed, with `hpx_main` left on locality 0 alone, which is the shape this program is written for -- the interpolator is one client there, and it is that client which spreads the partitions over the localities and spawns every worker
- markers: `[HPX]` geometry from every locality (a startup function, since `hpx_main` reaches only locality 0), `[E2E]` from the first statement of `hpx_main`'s body -- so the interpolator's construction, which reads the whole table off disk into the partitions, is inside the window -- to the completion edge, where every worker's future has been waited on; `[PARCELS]` after the end stamp when the structural marker is set.  The origin's own `high_resolution_timer` is restarted after the interpolator is built and so times the queries alone; it stays exactly where it is, and the marker window deliberately does not exclude what it excludes, because the mirror reads the same table inside its own program and the runtime's stamp cannot exclude that either
- scalar: the program discards what it computes (`std::vector<std::vector<double>> results` is assigned and cast to void), so the edit keeps it: `test_sheneos_bulk` returns the sum of the values it received, in the order of the query array it built, and `hpx_main` sums the one value per worker and prints `CHECKSUM %.14g` by one write.  The tally runs after the end stamp, so it is teardown on both sides.  The two dead test functions (`test_sheneos`, `test_sheneos_one_bulk`) are left exactly as they are -- nothing calls them
- bugfix: `fill_partitions` initialised the partitions through an EMPTY vector.  `partitions_` is assigned from `hpx::new_<partition3d[]>`'s future only after the triple loop that calls `partitions_[index].init_async(...)`, so every one of those calls indexed a default-constructed, zero-length vector -- undefined behaviour, guarded only by an `HPX_ASSERT(index < partitions_.size())` that a release build compiles out.  The defect is demonstrable from the source alone and the repair does not rest on having watched it fail: the triple loop's `partitions_[index].init_async(...)` runs while `partitions_ = result.get();` still sits below it, so the vector is empty at every one of those indexings.  The assert states the intended precondition and the only way to satisfy it is to have the vector before the loop, so `partitions_ = result.get();` moves above it.  Reverting this one hunk and rebuilding does crash the program at one locality, which is the symptom and not the basis: the undefined behaviour is what licenses the repair, and it is visible in the source whether or not a given build happens to fault.  Nothing else changes: the same partitions are created on the same localities, initialised with the same slabs, and registered under the same names.  The origin's own build file records the state this fixes -- `examples/sheneos/CMakeLists.txt` carries `# TODO: Fix example. Not added to unit tests until fixed.`

The component's own `CMakeLists.txt` is not a source and is not carried; the
program is built by the app project from the six translation units.
`interpolator.cpp` already registers a component module, so the component name
the app project gives each executable needs no edit of its own here.
`sheneos_client.cpp` is a second program of the same example and is not copied.
