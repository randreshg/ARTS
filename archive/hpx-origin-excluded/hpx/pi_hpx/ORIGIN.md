# pi_hpx (distributed_pi)

origin: third_party/hpx/libs/full/collectives/examples/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Hartmut Kaiser, 2025)
files:
- `libs/full/collectives/examples/distributed_pi.cpp` -> `distributed_pi.cpp`
edits:
- cfg: `hpx_main.hpp` (built upstream with HPX_HAVE_RUN_MAIN_EVERYWHERE) replaced by `hpx::init` with the runtime defaults, which run hpx_main on every locality the same way
- markers: `[HPX]` from every locality before the broadcast, `[E2E]` opened after the broadcast that gives every locality N, closed after the reduce, `[PARCELS]` after the end stamp when the structural marker is set
- scalar: the `pi:` line printed with 15 significant digits by a single write (`arts_hpx::write_stdout_line`, avoiding interleaving with the unbuffered `[E2E]` write), rather than the origin's default stream precision of 6 over multiple `<<` writes
