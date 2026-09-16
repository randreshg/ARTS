# random_mem_access_hpx (random_mem_access)

origin: third_party/hpx/examples/random_mem_access/ (HPX v1.11.0, commit c9b81b401fbe6ba4a0306feff71d18ef01c71b34)
pin: v1.11.0
licence: BSL-1.0 (Matt Anderson, Bryce Lelbach, Hartmut Kaiser, 2007-2015)
files:
- `examples/random_mem_access/random_mem_access_client.cpp` -> `random_mem_access_client.cpp`
- `examples/random_mem_access/random_mem_access/random_mem_access.cpp` -> `random_mem_access/random_mem_access.cpp`
- `examples/random_mem_access/random_mem_access/random_mem_access.hpp` -> `random_mem_access/random_mem_access.hpp`
- `examples/random_mem_access/random_mem_access/server/random_mem_access.hpp` -> `random_mem_access/server/random_mem_access.hpp`
edits:
- stdout: the per-action stream output in init/add/query/print is removed (each action printed a line)
- CLI: `--seed` (default: a random_device draw, as the origin seeds)
- ordering: the init loop waits for its posts (the origin's fire-and-forget init races its adds on the same component; the oracle needs init first)
- scalar: the print phase becomes a query gather on locality 0 printing `COUNT_SUM <sum of final counts minus sum of initial counts>`
- markers: `[HPX]` from every locality (startup function), `[E2E]` around the whole program body of hpx_main, `[PARCELS]` after the end stamp when the structural marker is set
- cfg: the runtime defaults without run_hpx_main (hpx_main is locality 0's driver; the components live everywhere)
