# HPX ports of OCR programs (retired)

Six HPX programs that mirrored OCR applications tier for tier: `nqueens`,
`p2p`, `Stencil2D_intel_channelEVTs` (`stencil2d.cpp`), `smithwaterman`,
`triangle`, `tempest`, each with its algorithm core (`*_core.hpp`), the core's
unit test, the structural-line checker, the README that documented them
(`README-ports.md`) and the `hpx-vs` quick-comparison roster.

Retired 2026-09-11. The comparison was built the wrong way round: holding the
OCR program fixed selects the roster by what HPX can express (only sharing
that is event-ordered at data-block granularity), and every port had to add
by hand the ordering the OCR runtime supplies. The replacement takes HPX
programs as the fixed side and mirrors them in OCR, where the placement and
movement an HPX program states can always be carried.

What these ports established and the successors keep:

- The vendored runtime's thread-queue backends compile only with 128-bit
  atomics, and an eagerly spawned tree needs the LIFO pending queue
  (`hpx.scheduler=local-priority-lifo`) to keep its frontier from becoming
  a stack per task; see `benchmarks/hpx/common/runtime_defaults.hpp`.
- A process's mapping count follows the thread objects it ever materialised
  (stack + guard page each), so the per-queue ceiling is a budget knob.
- Colocated ranks on one host need `UCX_TLS=^sysv` under MPICH/UCX.
- The `[E2E]` window convention (`common/e2e.hpp`): open after the
  readiness collective, close at the completion edge.
- Per-message calibration: one ARTS 8-byte remote acquire against one HPX
  action round trip measured 1.28× (`probes/action_rtt.cpp`,
  `tests/ocr/acquire_rtt.c`).

Reviving a port needs `add_hpx_port(NAME … SOURCES … [HINTED])` back in the
app project and the row's `hpx:` list back in the catalog; nothing else
referenced them.
