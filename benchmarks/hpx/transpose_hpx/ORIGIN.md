# transpose_hpx (transpose_block)

origin: third_party/hpx/examples/transpose/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Thomas Heller, 2014)
files:
- `examples/transpose/transpose_block.cpp` -> `transpose_block.cpp`
edits:
- build: the program registers its own component module, whose factory and registry plugin lists its component registration refers to (the app project gives each executable its own `HPX_COMPONENT_NAME`, so the runtime's default module is not the one it names)
- cfg: the run-everywhere cfg vector becomes the runtime defaults, which carry the same `hpx.run_hpx_main` line
- markers: `[HPX]` geometry from every locality, `[E2E]` from the first application statement of `hpx_main`'s body -- the block allocation -- to the completion edge (the root's accumulated error in hand), `[PARCELS]` after the end stamp when the structural marker is set.  The origin's own `high_resolution_timer` is per iteration and stays exactly where it is; it never covered the block creation, the fill or the basename rendezvous, which the marker window holds
- scalar: the squared error the origin already accumulates is printed as `ERRSQ %.6e` by one write, unconditionally, where the origin shows it only under `--verbose`
