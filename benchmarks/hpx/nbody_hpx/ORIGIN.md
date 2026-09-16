# nbody_hpx (DistributedNBody)

origin: third_party/NBody/DistributedNBody/ (STE||AR-GROUP/NBody, commit 6147a1763e8f8e62bfb836c342055514703a8305, 2016-02-17)
pin: 6147a1763e8f8e62bfb836c342055514703a8305
licence: none stated in the repository -- no licence file, and no notice in
  the source
files:
- `DistributedNBody/NBody Code/main.cpp` -> `main.cpp`
edits:
- measurement: APP_E2E starts immediately before do_work and ends after the
  two final read passes. The earlier E2E clock retains its original boundary
  before tree setup; both observations remain separate.
- modernise: the program is written against the 2016 interfaces and is
  compiled against 1.11, so most of the patch is renames with no change of
  meaning: `hpx/lcos/gather.hpp` becomes `hpx/modules/collectives.hpp` and
  `hpx::lcos::gather_here`/`gather_there` become
  `hpx::collectives::gather_here`/`gather_there` with `num_sites_arg`;
  `HPX_REGISTER_GATHER` is still spelled that way and stays; the
  `hpx/runtime/serialization/...` includes become their module paths;
  `hpx/include/parallel_algorithm.hpp` becomes `hpx/algorithm.hpp`, and with
  it `hpx::parallel::for_each` becomes `hpx::for_each` and the policy comes
  from `hpx::execution`; `boost::counting_iterator` becomes
  `hpx::util::counting_iterator`; `boost::program_options` becomes
  `hpx::program_options`; `boost::uint64_t` becomes `std::uint64_t`;
  `boost::format` becomes `hpx::util::format`, whose conversion goes inside the
  placeholder rather than beside it (`"{:.14g}"`); the `boost/shared_array.hpp`
  include is dropped, nothing having used it -- those are every Boost use in
  the file, so no Boost remains; `hpx::lcos::local::spinlock` becomes
  `hpx::spinlock` and its `scoped_lock` a `std::lock_guard`;
  `hpx::components::simple_component_base` and `simple_component` become
  `component_base` and `component`; `hpx::lcos::local::dataflow` becomes
  `hpx::dataflow` and `hpx::util::unwrapped` becomes `hpx::unwrapping`;
  `hpx::apply` becomes `hpx::post`; `hpx::find_id_from_basename` and
  `hpx::register_id_with_basename` become `find_from_basename` and
  `register_with_basename`; `get_gid()` becomes `get_id()`;
  `hpx::get_num_localities_sync()` becomes
  `hpx::get_num_localities(hpx::launch::sync)`;
  `hpx::util::high_resolution_clock` becomes
  `hpx::chrono::high_resolution_clock`; a serialized buffer now invokes a
  deleter it has taken through a const reference, so the deleter that keeps a
  referenced partition alive is `const`; the `hpx::init(argc, argv, cfg)` call
  becomes `hpx::init_params` plus `hpx::init(argc, argv, init_args)`; and
  headers the older tree pulled in transitively are named where they are used.
  One of these is not only a spelling: the collective now takes the local
  VALUE where the old one took a future, so a locality resolves its own result
  at the call (`gather_here(..., result.get(), ...)`).  That is one more wait
  in the driver body, beside the waits the program already has there, and none
  inside a task
- cli: the six file-scope constants `n`, `nt`, `SIZE`, `theta`, `th`, `k_th`
  and the cell count become `--n --nt --size --theta --th --k-th --np`, each
  defaulting to the value the file gives it.  `--np` defaults to 0, which
  selects the octree level the way the program selects it: 8 below nine
  localities and 64 from nine.  A value the cell map cannot be read with is
  refused as usage -- the map is written for those two levels alone, and it
  leaves a locality no cell once there are more localities than cells.
  `hpx_main` gains an `options_description` and `main` passes it through
  `init_params`, which the `hpx::init(argc, argv, cfg)` call does not do.
  Two arguments the constants never reached are refused before the run
  starts, since exposing them is what made them reachable: a `--size` below
  `--k-th`, which would index a partition before its own start where a step
  splices arrivals into its tail, and a `--size` below the widest cell of the
  level, whose members a partition is filled with.  Both are read off the tree
  every locality has already built, and both are computed over the cells the
  localities own rather than over one locality's share, so every locality
  reaches the same verdict rather than one refusing while the others wait
  for it
- cfg: the run-everywhere cfg vector becomes the runtime defaults, which carry
  the same `hpx.run_hpx_main` line
- markers: `[HPX]` geometry from every locality once the locality count is
  known (every locality runs `hpx_main`); `[E2E]` from the top of `hpx_main`,
  after the options are read and before `create_Octree()` -- so the octree
  build is inside the window -- to after locality 0 has pulled every gathered
  cell's data, with `[PARCELS]` after the end stamp when the structural marker
  is set.  The program's own `high_resolution_clock` starts AFTER the octree
  build, and its value is still printed exactly where it was -- but that report
  ends without a newline, so a newline is flushed after it.  Every marker and
  the scalar are then a whole line of their own, which is what a stream merged
  from several localities can be read line by line
- scalar: the program computes no printable result of its own, so the edit is
  the simplest digest of the final state it already holds.  Locality 0 already
  pulls every gathered cell's data in a loop of its own; the sum of `r1[0]`,
  `r1[1]` and `r1[2]` over every body of every gathered cell is accumulated in
  that loop and written once as `CHECKSUM %.14g`.  What it can and cannot see
  is in the row's appdoc: a partition carries the positions its bodies had when
  it was built -- `compute_position` writes only the force back into the
  partition it passes on -- so the digest follows the bodies as they move
  between cells and localities, and is blind to the force kernel, which writes
  nowhere else
- build: the program defines two components, and the app project gives each
  executable a component name of its own, so the component module the factory
  registrations refer to is registered here
  (`HPX_REGISTER_COMPONENT_MODULE()`) rather than borrowed from the runtime's
  default module
- bugfix: `hpx_main` declares `int np;` and assigns it only below nine
  localities (8) and from sixteen to sixty-four (64).  At nine to fifteen
  localities -- and above sixty-four -- the cell count is therefore read
  uninitialised: undefined behaviour, and a distribution over a number of
  cells nobody chose.  The `--np` default above closes the gap by the rule the
  two assigned branches state, and the usage check refuses what is left
- bugfix: `idx()`, which names a locality's neighbour in one of six
  directions, declares `int temp;` in each of its two lower blocks and assigns
  it only for the locality counts it tabulates -- 8, 16, 32 and 64, and
  through a separate branch the counts below eight and below four.  At every
  other count (five to seven, nine to fifteen, seventeen to thirty-one, and so
  on) the value is read uninitialised and the neighbour is not a locality.
  `temp` is initialised to `i`, which is the stay-put answer the function
  itself gives for the counts below its smallest tabulated one.  The change is
  a no-op wherever the function does assign, so it cannot move an answer at
  any locality count the program tabulates.  The function also runs off its
  end without a return -- the compiler says so on every build -- which the
  same stay-put value closes; none of the six directions the program asks for
  reaches that path, so this one cannot move an answer at all
- bugfix: the same function's sixty-four-locality block tests the locality
  INDEX where every branch beside it tests the direction: `i==-2` and `i==+2`
  on a `std::size_t` index, which no locality can equal, and which the `i<4`
  and `i>size-5` beside them make impossible a second time.  Both tests are
  therefore dead, and with them dead that block falls through to
  `temp=i+(2*dir)`, which at sixty-four localities steps below zero for the
  first four indices and returns the wrap through an unsigned return type --
  a "neighbour" no locality registers and the program then waits for, the
  assertion that would have caught it being compiled out of a release build.
  The two tests become `dir==-2` and `dir==+2`, which is what the `±4` block
  below them spells for the same intent, and the repaired branches make each
  direction a permutation of the localities: `-2` sends the first four indices
  to the last four and every other index four places down, `+2` the last four
  to the first four and every other index four places up
- bugfix: `cell_list()` walks the tree once for every cell that holds bodies
  of its own and appends what it finds to `cell[b[i].parent].list_cell1` and
  `list_cell2`, under `hpx::parallel::for_each(par, ...)`.  Those target cells
  are not distinct: with 20 000 bodies and a subdivision threshold of 100, 64
  iterations write 41 cells and 18 of the 41 are written by two or three
  iterations at once, each an unsynchronised `push_back` on the same
  `std::vector`.  That is a data race, so the interaction lists -- and every
  force computed from them -- are undefined and no two runs need agree.  The
  policy of that one algorithm becomes `hpx::execution::seq`.  It is the least
  that restores both defined behaviour and a defined answer, and it is not a
  different answer from the one the program means: appending in iteration
  order is what a per-iteration buffer merged afterwards would produce.  What
  it costs is that loop's own parallelism, and the loop is inside the measured
  window: the traversals now run one after another.  The mirror builds the
  same lists serially too, so the two sides stay symmetric in what the window
  covers

The tree's own `CMakeLists.txt` is not a source and is not carried; the app
project builds the one translation unit it names.  Neither is `README.txt`,
the third file of the origin's directory -- but it is the origin's only
statement about the arguments the `cli` edit exposes, so its rule is carried
into the row's appdoc instead: `n` is the particle count, `nt` the iteration
count, and `SIZE = 15 * ( n / (number of localities) )`.

What is left exactly as
written is in the row's appdoc: the force kernel's `1 +` per interaction and
its `float` distance, the `std::rand()` stream that is never seeded (the
determinism both sides of the comparison rest on), the fixed
`std::vector<Cell> cell(1000000)` every rank allocates whatever `n` is, the
five of each step's six `New_Members` whose results are overwritten before
anything reads them, and `neighbor()`, which nothing calls.
