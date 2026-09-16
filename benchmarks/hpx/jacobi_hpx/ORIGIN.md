# jacobi_hpx (jacobi)

origin: third_party/hpx/examples/jacobi/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Thomas Heller, 2012)
files:
- `examples/jacobi/jacobi.cpp` -> `jacobi.cpp`
- `examples/jacobi/jacobi_component/grid.hpp` -> `jacobi_component/grid.hpp`
- `examples/jacobi/jacobi_component/grid.cpp` -> `jacobi_component/grid.cpp`
- `examples/jacobi/jacobi_component/row.hpp` -> `jacobi_component/row.hpp`
- `examples/jacobi/jacobi_component/row.cpp` -> `jacobi_component/row.cpp`
- `examples/jacobi/jacobi_component/row_range.hpp` -> `jacobi_component/row_range.hpp`
- `examples/jacobi/jacobi_component/stencil_iterator.hpp` -> `jacobi_component/stencil_iterator.hpp`
- `examples/jacobi/jacobi_component/stencil_iterator.cpp` -> `jacobi_component/stencil_iterator.cpp`
- `examples/jacobi/jacobi_component/solver.hpp` -> `jacobi_component/solver.hpp`
- `examples/jacobi/jacobi_component/jacobi_component.cpp` -> `jacobi_component/jacobi_component.cpp`
- `examples/jacobi/jacobi_component/server/row.hpp` -> `jacobi_component/server/row.hpp`
- `examples/jacobi/jacobi_component/server/row.cpp` -> `jacobi_component/server/row.cpp`
- `examples/jacobi/jacobi_component/server/stencil_iterator.hpp` -> `jacobi_component/server/stencil_iterator.hpp`
- `examples/jacobi/jacobi_component/server/stencil_iterator.cpp` -> `jacobi_component/server/stencil_iterator.cpp`
- `examples/jacobi/jacobi_component/server/solver.hpp` -> `jacobi_component/server/solver.hpp`
- `examples/jacobi/jacobi_component/server/solver.cpp` -> `jacobi_component/server/solver.cpp`
edits:
- cfg: the runtime defaults are installed, with `hpx_main` left on locality 0 alone, which is the shape this program is written for -- the solver is one component there and its `run` drives every locality's iterators
- markers: `[HPX]` geometry from every locality (a startup function, since `hpx_main` reaches only locality 0), `[E2E]` from the first statement of `hpx_main`'s body -- so the grid and the solver construction, which allocate and initialise the whole state, are inside the window -- to the completion edge, where `run` has joined the last iteration on every locality; `[PARCELS]` after the end stamp when the structural marker is set.  The origin's own `high_resolution_timer` is inside `solver::run` and stays exactly where it is; it never covered the grid or the solver construction, which the marker window holds
- scalar: the program computes no printable quantity of its own (it prints a rate), so the edit adds the simplest digest of the final state: an iterator sums its own current row and the solver totals those sums, printed as `CHECKSUM %.14g` by one write.  The gather runs after the origin's timer and after the end stamp, so it is outside the measured window on both sides and costs one action per row in teardown only

The component tree's own `CMakeLists.txt` is not a source and is not carried;
the program is built by the app project from the seven translation units.
`jacobi_component.cpp` already registers a component module, so the component
name the app project gives each executable needs no edit of its own here.
