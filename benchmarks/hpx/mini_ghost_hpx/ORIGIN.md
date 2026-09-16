# mini_ghost_hpx (MiniGhost)

origin: third_party/miniapps/MiniGhost/src/ (STE||AR-GROUP/miniapps, commit 0cf9c1c127b88747b6e631b119dff9021942eb13, 2015-10-22)
pin: 0cf9c1c127b88747b6e631b119dff9021942eb13
licence: BSL-1.0 (Thomas Heller, Hartmut Kaiser, 2013-2014)
files:
- `MiniGhost/src/barrier.hpp` -> `barrier.hpp`
- `MiniGhost/src/global_sum.hpp` -> `global_sum.hpp`
- `MiniGhost/src/grid.hpp` -> `grid.hpp`
- `MiniGhost/src/mini_ghost.cpp` -> `mini_ghost.cpp`
- `MiniGhost/src/pack_buffer.hpp` -> `pack_buffer.hpp`
- `MiniGhost/src/params.hpp` -> `params.hpp`
- `MiniGhost/src/partition.hpp` -> `partition.hpp`
- `MiniGhost/src/profiling.hpp` -> `profiling.hpp`
- `MiniGhost/src/recv_buffer.hpp` -> `recv_buffer.hpp`
- `MiniGhost/src/send_buffer.hpp` -> `send_buffer.hpp`
- `MiniGhost/src/spikes.hpp` -> `spikes.hpp`
- `MiniGhost/src/stencils.hpp` -> `stencils.hpp`
- `MiniGhost/src/stepper.cpp` -> `stepper.cpp`
- `MiniGhost/src/stepper.hpp` -> `stepper.hpp`
- `MiniGhost/src/unpack_buffer.hpp` -> `unpack_buffer.hpp`
- `MiniGhost/src/write_grid.cpp` -> `write_grid.cpp`
- `MiniGhost/src/write_grid.hpp` -> `write_grid.hpp`
edits:
- modernise: the program is written against the 2014 interfaces and is compiled
  against 1.11, so most of the patch is renames with no change of meaning:
  `hpx/hpx_fwd.hpp` is gone and its includes are dropped; module include paths
  replace the old `hpx/lcos/...`, `hpx/util/...` and `hpx/runtime/...` ones;
  `boost::program_options` becomes `hpx::program_options` (the only Boost
  library this build does not carry -- the accumulators, date_time, random,
  serialization and lexical_cast uses are unchanged, so the random draws and
  the report are the program's own); `hpx::util::high_resolution_timer`
  becomes `hpx::chrono::high_resolution_timer`; `hpx::lcos::local::spinlock`
  and its `scoped_lock` become `hpx::spinlock` and `std::lock_guard`;
  `hpx::lcos::local::counting_semaphore` becomes
  `hpx::counting_semaphore_var<>`, which keeps `wait`/`signal`;
  `hpx::lcos::local::promise` becomes `hpx::promise`;
  `hpx::lcos::local::dataflow` becomes `hpx::dataflow`; `hpx::util::bind` and
  `boost::ref` become `hpx::bind` and `std::ref`; `hpx::util::unwrapped2`
  becomes `hpx::unwrapping_n<2>`; `hpx::apply` becomes `hpx::post`;
  `hpx::get_num_localities_sync()` becomes
  `hpx::get_num_localities(hpx::launch::sync)`; `this->get_gid()` becomes
  `this->get_id()`; `boost::shared_ptr` becomes `std::shared_ptr`;
  `HPX_REGISTER_MINIMAL_COMPONENT_FACTORY` becomes `HPX_REGISTER_COMPONENT`;
  the broadcast of the sum action becomes `hpx::lcos::broadcast_post` with the
  matching registration macros (one parcel per target either way);
  `HPX_MOVABLE_BUT_NOT_COPYABLE` is gone, so the three classes that used it
  delete their copy operations and keep their own move operations; the
  `hpx::init(desc, argc, argv, cfg)` call becomes `hpx::init_params` plus
  `hpx::init(argc, argv, init_args)`; the barrier is a named
  `hpx::distributed::barrier` constructed on every locality instead of one
  created at rank 0 and looked up through an AGAS symbol event; the
  all-reduce gate takes its participant count at construction and advances its
  generation explicitly, because the interface no longer takes the count with
  every generation; and headers the older tree pulled in transitively are named
  where they are used
- build: the program defines components, and the app project gives each
  executable a component name of its own, so the component module the two
  factory registrations refer to is registered here
  (`HPX_REGISTER_COMPONENT_MODULE()`) rather than borrowed from the runtime's
  default module
- cfg: the run-everywhere cfg vector becomes the runtime defaults, which carry
  the same `hpx.run_hpx_main` line
- markers: `[HPX]` geometry from every locality after `p.setup(vm)` (every
  locality runs `hpx_main`), `[E2E]` from immediately before `stepper->init(p)`
  -- so the stepper's initialisation, both barriers and the run are inside the
  window -- to after the second `barrier_wait()`, with `[PARCELS]` after the end
  stamp when the structural marker is set.  The origin's own
  `high_resolution_timer`s stay where they are; `timer_all` in `hpx_main` starts
  earlier (before the locality queries and the component creation) and its value
  is printed only when `report_perf` is off
- scalar: the origin already computes `error_iter`, the relative error between
  the analytically injected total and the global sum, once per step per summed
  variable, and already terminates when it exceeds `error_tol`.  A file-scope
  `std::atomic<double> g_err_max` keeps the largest of those values on the
  locality that computes them (rank 0), and after the second barrier rank 0
  prints `ERRMAX %.6e` by one write.  The atomic is what makes the running
  maximum well defined: the summed variables' checks run concurrently
- bugfix: under strong scaling the remainder of the global dimension over the
  process-grid dimension is computed with `&` instead of `%`
  (`remainder = tmp_nx & npx`, and likewise for y and z), which is not a
  remainder at all -- it is a bitwise and, and it gives a wrong local dimension
  for most dimension/grid pairs (for example 100 & 3 = 0 where 100 % 3 = 1).
  The three operators are corrected.  The `if(rank < remainder)` beside them is
  left exactly as it is, and deliberately not judged: whether the remainder
  belongs against the linear rank or against the rank's coordinate in that
  dimension cannot be settled from anything in this tree, which carries the HPX
  port and no reference implementation
- bugfix: `Real source_total_` and `Real flux_out_` are members of a class with
  a user-provided constructor whose init list initialises neither, so both are
  indeterminate, and the first thing done to `flux_out_` is a read: `flux_out_
  += flux_out_future.get()` inside `sum_grid`, whose result the conservation
  check is computed from.  Reading an indeterminate `double` is undefined
  behaviour, and it is reachable exactly where the check is -- with the default
  `percent_sum` of 0 `sum_grid` returns before that line, which is why it
  survives in the published program.  It is not theoretical: at eight
  localities four runs in twenty reported errors of 7.5e+19, 8.4e+92, 9.3e+226
  and 13.8 at the first step and terminated.  Both members are initialised to
  zero in the init list
- bugfix: each direction's send is gated on the chunk at the opposite end of
  its axis from the plane it packs -- `WEST` packs `g(1,y,z)`, the plane the
  first x-chunk writes, and waited for the last (`nx_block ==
  calc_futures[dst_].nx_-1`), `EAST` packs `g(nx_-2,y,z)` and waited for the
  first, and so on for all six.  The chunks of one step are mutually unordered,
  so the send could read a plane this step had not yet written and ship the
  previous generation's values: a race whose outcome depends on scheduling.
  Where an axis has one chunk the gate lands on the writer and nothing happens,
  which is why it is invisible until an axis is cut into several; with
  `npz > 1` and `nz_block` below the local depth it made every run of an
  eight-locality job return a different answer (twenty runs, twenty distinct
  values, on the HPX program and on the mirror alike).  Each direction now
  waits for the chunks that write the plane it packs
- bugfix: a partition attaches a continuation that captures its own address --
  it stores the global sum of its initial grid into `source_total_` -- and the
  stepper then built each partition as a temporary and moved it into its
  vector (`partitions_.push_back(partition_type(...))`).  The partial sums
  arriving later reach the object in the vector, while the continuation reads
  the value of the object that was moved out of and writes `source_total_`
  through a pointer to it after it has been destroyed: undefined behaviour,
  and in effect `source_total_` stays zero, so every summed variable's
  conservation check fails on its first step and the program calls
  `hpx::terminate()`.  (The default `percent_sum` is 0, so no variable is
  summed and the check never runs -- which is how the defect survives in the
  published program.)  The partition is constructed in place instead, in the
  vector whose storage is already reserved, so the address the continuation
  captured is the one that receives the sums
- bugfix: a pack reads a full padded plane of `grids_[dst_]` -- the halo rows
  and the corners included -- and the unpacks of the step after it write the
  halo planes of that same array, which is `grids_[src_]` by then.  Nothing
  orders the two: `partition::run` issues every step without blocking, so an
  unpack exists from the start and its only gate is the neighbour's pack of the
  step before, while the future every send returns is dropped where it is made
  (`when_all(...).then(...)` with nothing to receive it) and the `wait_all` that
  closes the loop names every other future.  Where a rank has neighbours on
  more than one axis the two zones meet on the box's edge lines, so whether a
  packed edge line carries the plane this step wrote or the halo the next step
  has already unpacked into it depends on scheduling.  Where a rank has
  neighbours on one axis only the zones are disjoint and nothing happens, which
  is why it survives in the published program.  Each step's send futures are
  kept, and every unpack waits for all of its own rank's sends of the step
  before it before it copies; the receive itself stays inside the unpack task,
  so nothing else about the order in which the loop issues its work changes

The tree's own `CMakeLists.txt` files are not sources and are not carried; the
app project builds the three translation units and the headers they include.
The `-DHPX_LIMIT=10` its build file set is not carried either -- that limit no
longer exists in this HPX.  `results.yaml`, which the origin writes at the end
of a run, is left as written; runs happen from `scratch/`, so it lands in the
gitignored root.
