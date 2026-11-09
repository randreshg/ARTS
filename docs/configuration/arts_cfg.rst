Runtime Configuration (``arts.cfg``)
====================================

ARTS reads its runtime configuration from ``arts.cfg`` in the current
working directory.  Override the path with the ``ARTS_CONFIG`` environment
variable:

.. code-block:: bash

   ARTS_CONFIG=/path/to/custom.cfg ./my_program

.. contents:: On this page
   :local:
   :depth: 2

Threading
---------

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``worker_threads``
     - 4
     - Worker threads per node: the threads that run EDTs; a rank needs at
       least one.  The progress thread(s) that drive the network are
       separate — see ``progress_threads`` below.  Under ``launcher=slurm``
       with ``SLURM_CPUS_PER_TASK`` set, that variable is the rank's thread
       count and the workers are what ``progress_threads`` leaves of it, so
       this key has no effect.  A rank left with no worker is refused at
       load: ``worker_threads=0`` (unless a single-node run reclaims progress
       threads as workers), or ``progress_threads`` taking every thread
       ``SLURM_CPUS_PER_TASK`` gives.
   * - ``stack_size``
     - 0
     - Thread stack size in bytes (0 = OS default).

Hardware Pinning
----------------

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``pin``
     - 1
     - Enable thread-to-core pinning.

Scheduling
----------

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``scheduler``
     - 0
     - Scheduler loop, by where a worker looks for its next EDT: ``0`` takes
       its own deque, then makes one steal attempt on a random other
       thread's deque (progress threads included), then scans the progress
       threads' deques; ``1`` its own deque, then the progress threads', then
       one random steal; ``2`` the progress threads', then its own, then one
       random steal.  A GPU build adds its GPU loops at ``3``-``5``.  An
       index the build does not have is refused at load.
   * - ``worker_init_deque_size``
     - 4096
     - Initial size of each worker's Chase-Lev deque, at least 1; ``0`` is
       refused at load.
   * - ``route_table_size``
     - 16
     - Log2 of the windows in the first segment of each of a rank's route
       tables, 1..31; a value outside is refused at load.  A window is 8
       slots of 64 bytes, so a first segment is 2\ :sup:`size+9` bytes (32 MiB
       at 16, 8 MiB at 14), and a rank keeps T route tables, T being its
       thread count (its worker and progress threads together).  A segment
       is mapped without reserving memory: a page becomes resident when a
       slot on it is first written and stays resident until the run ends.  A
       table that needs more room links a segment twice the size of its
       last.  A key's table follows from the key alone: each source's keys of
       one kind fill half a first segment (2\ :sup:`size+2` keys) in one
       table, then move to the next, so one source's large set spreads over
       every table and one thread's long stream of short-lived objects can
       leave all T first segments resident.

GPU Support
-----------

To enable GPU support, set ``scheduler=3`` and ``gpu=<N>``.  The four
device-memory reclamation keys (``free_db_after_gpu_run``,
``run_gpu_gc_idle``, ``run_gpu_gc_pre_edt``, ``delete_zeros_gpu_gc``) are
parsed but have no effect: the per-DataBlock return and the
garbage-collection sweep they gate are not implemented.

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``gpu``
     - 0
     - Number of GPUs per node.
   * - ``gpu_route_table_size``
     - 12
     - GPU routing table size (power of 2), 1..28; a value outside is refused
       at load.
   * - ``gpu_locality``
     - 0
     - Locality policy: 0 = random, 1 = allOrNothing, 2 = atLeastOne.
   * - ``gpu_fit``
     - 0
     - Fit policy: 0 = firstFit, 1 = bestFit, 2 = worstFit,
       3 = roundRobinFit.
   * - ``gpu_max_edts``
     - -1
     - Max concurrent GPU EDTs (-1 = unlimited).
   * - ``gpu_max_memory``
     - -1
     - Max GPU memory in bytes (-1 = unlimited).
   * - ``gpu_p2p``
     - 0
     - Enable GPU peer-to-peer transfers.
   * - ``gpu_lc_sync``
     - 0
     - Location Consistency sync policy: 0 = artsGetLatestGpuDb,
       1 = artsGetRandomGpuDb.
   * - ``gpu_buff_on``
     - 0
     - Enable GPU stream buffering.
   * - ``free_db_after_gpu_run``
     - 0
     - Mark a DataBlock's device copy for deletion when the GPU EDT that
       used it completes, instead of keeping it for reuse.  The per-DataBlock
       return is not implemented, so the key has no effect.
   * - ``run_gpu_gc_idle``
     - 1
     - Run the GPU garbage-collection sweep when a worker is idle.  The
       sweep is not implemented, so the key has no effect.
   * - ``run_gpu_gc_pre_edt``
     - 0
     - Run the GPU garbage-collection sweep before each GPU EDT executes.
       The sweep is not implemented, so the key has no effect.
   * - ``delete_zeros_gpu_gc``
     - 1
     - Let the GPU garbage-collection sweep delete DataBlocks with no
       remaining references.  The sweep is not implemented, so the key has
       no effect.

Networking
----------

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``progress_threads``
     - 1
     - Number of progress threads per node, draining the fabric completion
       queue (a single-node run has none: its count goes to the workers).
       The transport injects sends directly from workers.
   * - ``provider``
     - auto
     - libfabric provider name passed to ``fi_getinfo``'s ``prov_name`` hint
       (``tcp``, ``verbs;ofi_rxm``, ``cxi``). Unset/empty auto-selects.
       InfiniBand must be spelled as the layered set ``verbs;ofi_rxm`` (the
       verbs core is MSG-only, so a bare ``verbs`` can never match the
       runtime's RDM request); ``cxi`` (HPE Slingshot) is native RDM and is
       named bare — naming it is also what commits the runtime's cxi
       adaptations before matching, so auto-selection never reaches it. When
       set, this overrides the ambient ``FI_PROVIDER`` environment variable
       for the process — the config is the deliberate, versioned artifact;
       the environment variable is ambient and host-specific.
   * - ``net_interface``
     - unset
     - Interface whose first IPv4 address the endpoint's source address is
       bound to (exact name, else name prefix); an empty value is unset.
       Under ``tcp`` it steers the transport onto that network; left unset
       (and ``fabric_domain`` unset) under ``launcher=local``, every rank
       binds the loopback address, the one interface all ranks of a one-host
       run carry, since the provider's own pick follows the interfaces up at
       each process's start and two ranks that split across interfaces can
       reach each other only through address translation. Under
       ``verbs;ofi_rxm`` it makes every rank's fabric address the IPv4
       address of that interface (IPoIB, RoCE) and connections are resolved
       over it; left unset, the verbs stack is pinned to its native address
       format instead, which a fabric node offers whatever its interfaces
       carry. Either way every rank opens the same format — an address vector
       holds exactly one, and the provider on its own leads with a format
       that follows each node's interfaces. The fabric under ``cxi`` ignores
       the key. The bootstrap socket mesh (the fabric-address exchange, then
       the liveness sentinels) honours it under every provider, ``cxi``
       included: a rank first finds its own row of the node table, from the
       launcher's rank or else by the row a local interface carries, then
       shifts every node's resolved address by the one offset from its own
       row to the interface's address. That assumes the interface's network
       numbers the hosts as their resolved addresses do. A node list that
       already names that network is left as it is, and under
       ``launcher=local``, where every row is 127.0.0.1, every rank connects
       to the interface's address. On a run of more than one rank the
       bootstrap refuses to start, on every provider and before the fabric
       comes up, in three cases: the interface has no IPv4 address (the
       fabric checks that again wherever it binds to the interface); no
       local interface carries this rank's own row's address (a host name
       mapped to a loopback alias such as 127.0.1.1); or this rank's row is a
       loopback address other than the interface's own address (a host name
       mapped to 127.0.0.1, with an interface other than ``lo``) while another
       row is not loopback, since an offset from a loopback address says
       nothing about where the other hosts are.
   * - ``fabric_domain``
     - unset
     - libfabric domain name passed to ``fi_getinfo``'s domain hint (a NIC
       such as ``cxi0`` or ``mlx5_0``); unset lets the provider choose.
   * - ``regpool_slab_mb``
     - 256
     - Registered-memory base slab size in MB: the first slab carved for a
       NUMA node, on that node's first use. Backs the libfabric memory
       registration the DB payload buffers draw from. The pool grows on
       demand by half of the last slab mapped for the node, up to 16 GiB per
       slab; a payload above 32 MiB is served by a dedicated mapping kept for
       reuse, interleaved across the NUMA nodes this rank's threads are
       pinned to (one object every thread sweeps has no owning node), while a
       smaller payload comes from the allocating thread's own node. Placement
       is a preference throughout: a node that cannot take a mapping hands it
       to the nearest one that can.
   * - ``ports``
     - (see below)
     - The one port every rank listens on under a remote launcher, a number
       in 1-65535 (``25000``); a list or a range is refused.  **Required**
       with ``launcher=ssh``, ``slurm``, ``flux``, or ``lsf``, and
       **rejected** with ``launcher=local``.
   * - ``master_node``
     - first host of ``nodes``
     - Host of the master rank under ``launcher=ssh``.  The other launchers
       ignore it: under Slurm, Flux and LSF the first host of the job's own
       node list is the master, and a ``launcher=local`` run's is rank 0.

Who fixes the port depends on where the ranks land.  A remote launcher puts
one rank on each host, so every rank listens on the same port — but each of
them finds its peers' port from its own copy of this config, so the config
must name it.  Pick a port below the kernel ephemeral port range
(``net.ipv4.ip_local_port_range``, typically 32768–60999): a listen port inside
it collides at random with the source port of any outgoing connection the
machine makes, and a rank whose port stays taken fails its startup with a
``listen`` line (below).

A ``launcher=local`` run is the mirror image.  Every rank is a process on this
one machine, so they cannot share a port and the ports must be found rather
than declared: the process you start, which starts every rank and is none of
them, bind-probes a block of ``node_count`` consecutive ports, slides past
anything already holding one of them, and hands the block to the ranks it
starts; rank *i* listens on the block's first port plus *i*.  The search is
seeded from the process id so two runs starting at the same moment do not pick
the same block.  Naming ``ports`` there is a configure error — it would only
reintroduce the collisions the search exists to avoid.

At startup every rank of a run of several opens the bootstrap socket mesh,
one connection for each pair of ranks: the higher rank connects to the lower
one's port and names itself first, and the lower one accepts.  The ranks then
exchange their fabric addresses over these connections, which afterwards stay
open, carrying nothing, so that a peer's end closes them (see `Ending a Run`_).
A connection to the port that does not name a rank of the run is closed and
ignored.

Each rank has one **startup budget**, counted from the start of ``arts_rt``
to the moment its mesh and its address exchange are complete.  Within it a
connect that fails is retried 100 ms later, a connect that has not completed
after a second is issued afresh (so a peer that starts listening late is
reached within a second of it), and a port another process holds is tried
again every 100 ms.  The budget is ``ARTS_STARTUP_TIMEOUT_S``, a whole number
of seconds written as ASCII decimal digits, from 10 to 86400, and 20 when
unset.  Any other value, the empty one included, is refused with status 1 by
every process of the run, the spawner (below) before it starts any rank.  The
skew between the ranks of one job is the launcher's, not the runtime's, so the
budget comes from the environment rather than the config.

A rank that cannot complete its startup within the budget ends with status 75
and writes one line to standard error, at every log level::

   ARTS_MESH_FAILED rank=<r|-> host=<h> phase=<p> missing=<r,...|-> missing_hosts=<h,...|-> more=<k>

``rank`` is the rank, or ``-`` before the rank is known; ``host`` is the
rank's own name in the routing table, or, on a ``rank=-`` line, the node name
``uname`` reports.  ``phase`` names the step that failed:

- ``config``: loading the configuration, the launcher's query included;
- ``resolve``: resolving the routing table's names; ``missing`` lists the
  rows that did not resolve;
- ``listen``: this rank's own port, held by another process until the budget
  passed, or the host's file table too full for the rank's sockets;
- ``connect``: ``missing`` lists the peers whose connection never completed;
- ``fabric``: thread placement, the fabric's bring-up and the pool's
  registration, between the mesh and the exchange;
- ``exchange``: ``missing`` lists the peers whose addresses did not arrive;
- ``hangup``: ``missing`` lists the peers whose connection ended while that
  still proved they never started the program: before the exchange, or in it
  while a peer's addresses or this rank's addresses to it were incomplete.

``missing_hosts`` gives the listed ranks' routing-table names in the same
order.  At most 16 ranks are listed and ``more`` counts the rest; every other
phase, and every line of the startup timer below, reads
``missing=- missing_hosts=- more=0``.  The line is at most 2048 bytes and
written in one ``write``, so the lines of ranks that share a pipe never
interleave, and only if standard error takes it within a second.

A rank that gives up closes its connections first, so every rank connected to
it that still needed it (one still forming the mesh, or still lacking its
addresses) ends at once with a ``hangup`` line naming it: a ``hangup`` line
names a rank that gave up, not a cause.  A startup that fails by itself names
its cause in a line of another phase, and ``artsrun`` relaunches such a run
(``tools/artsrun/README.md``, "Mesh failures").  The run ends with 75, or with
a higher status a rank ended with (one killed by a signal, for instance).  A
rank stuck in a step that cannot wait on the deadline itself is ended a second
after it by the startup timer (see `Ending a Run`_), whose line names that
step.  A signal during startup ends the rank by the signal's own end, normally
without a line.

A configuration error is not a mesh failure: it ends the rank with status 1
and an error message instead of the line.  So does another error binding or
listening on the port, address bytes that are malformed or name a rank other
than the connection's, a process out of file descriptors (``EMFILE``), and a
descriptor limit the mesh cannot fit in.  Before its first connection a rank
counts the descriptors the mesh needs (one per peer, one for its port and
those already open) and, when the provider keeps a socket per peer (``tcp``,
which the automatic choice tries first), as many again; when the soft
``RLIMIT_NOFILE`` limit is below that, the rank raises it to the hard limit.
It refuses the run only when the mesh's own need does not fit: a hard limit
below it, or a soft limit below it that cannot be raised.

.. tip::

   To run several ranks on one machine, use ``launcher=local`` and let it
   place them.

Under ``launcher=local`` and ``launcher=ssh``, the process you start for a
run of more than one rank is the **spawner**: it starts every rank (a local
process, or an ``ssh`` client that runs the rank on its host), is none of them
and ends with the highest status a rank ended with.  Under ``slurm``,
``flux`` and ``lsf`` the scheduler starts the ranks and none is a spawner.  The
ranks inherit the spawner's standard output and standard error; nothing is
relayed.  Under ``launcher=local`` only rank 0 inherits its standard input
and every other rank reads ``/dev/null``; under ``launcher=ssh`` no rank has
standard input; the schedulers apply their own policy.  A rank that ends with
a non-zero status makes the spawner print
``[ARTS] spawner: rank <i> (pid <p>) ended with status <s>``.  A signal
(SIGINT, SIGTERM, SIGHUP or SIGALRM) sent to the spawner is passed on to
every rank, which ends as the section below says.

Under ``launcher=ssh`` the spawner runs each rank through
``sh -c`` on its host, in the spawner's working directory, and ends the rank if
the connection closes.  The rank receives every ``ARTS_*`` variable and every
variable named after a config key from the spawner's environment, with
``ARTS_CONFIG`` made an absolute path, ``ARTS_RANK`` and ``LD_LIBRARY_PATH``;
it does not receive ``FI_*``, ``MIMALLOC_*`` or sanitizer options, which must
be set on the host.  The login shell of every host must be POSIX-family
(``sh``, ``bash``, ``zsh`` or ``ksh``).

The ``nodes`` list carries hostnames only: each comma-separated entry is a
host name or a name followed by one bracketed number or range
(``node[01-10]``) whose bounds are unsigned ints.  Any other entry (one
holding ``:``, an empty entry, a malformed bracket) is refused at load
whatever the launcher.  Only ``launcher=ssh`` places ranks by the list, so
only there must a ``node_count`` agree with the number of nodes it names;
there, a config without a ``nodes`` line lists the one host ``localhost``, so
``launcher=ssh`` with ``node_count=4`` and no list is refused.  Under LSF the
first entry of ``LSB_HOSTS`` (or ``LSB_MCPU_HOSTS``) is taken as the launch
host and runs no rank, so a list that names no other host is refused.  Under
Slurm the list is ``SLURM_STEP_NODELIST`` (else ``SLURM_JOB_NODELIST``); an
environment with neither, or with a list naming fewer hosts than
``SLURM_NNODES``, is refused.  A scheduler's count (``SLURM_NNODES``,
``SLURM_CPUS_PER_TASK``, ``FLUX_JOB_NNODES``, ``FLUX_JOB_SIZE``) must be a
positive decimal integer, digits only; any other value is refused.

A numeric key (``worker_threads``, ``stack_size``, ``route_table_size``,
``node_count`` where the launcher reads it, and the rest outside the CXL
Store section) is read as decimal digits only.  An empty value, a sign, a
space, trailing text (``3x``), another base (``0x10``) or a value past the
key's type (an unsigned int; 64 bits for ``stack_size`` and
``gpu_max_memory``) is refused at load, naming the key and the value, and is
never read as some other number.  A boolean key (``pin``, ``core_dump``,
``gpu_p2p``, ``gpu_buff_on``, ``free_db_after_gpu_run``,
``run_gpu_gc_idle``, ``run_gpu_gc_pre_edt`` and ``delete_zeros_gpu_gc``) is
exactly ``0`` or ``1``.  The bounds of a host range (``node[01-10]``) follow
the same rule as an unsigned int.  A key's value comes from the file or from
an environment variable of the same name, and both are held to these rules.

Launcher selection is environment-driven for the schedulers that publish
their layout in the environment: under Slurm or LSF the runtime detects the
scheduler's variables and ignores the configured value.  ``launcher=flux``
is the one **opt-in** value: it must be named in the config, and the process
must then be a task of a Flux job — one task per node, e.g.
``flux run -N<node_count> -n<node_count> -c<width> <binary>`` — because the
runtime reads its rank from the task environment and its node roster from
the Flux CLI (Flux publishes no hostlist variable).  Naming ``flux`` outside
a Flux task environment is a hard error, never a silent fallback; and a
Flux variable leaking into a ``local`` or ``ssh`` run changes nothing.  A
config with **no** ``launcher`` line and no scheduler environment still
defaults to ``ssh``, Flux environment or not.  Outside a Slurm or LSF
environment the value must otherwise be ``local``, ``ssh`` or ``flux``;
``slurm`` or ``lsf`` there, or any other value, is refused at load.

Ending a Run
------------

A rank ends when one of its threads takes the rank's end: a call to
``arts_shutdown`` or ``arts_abort``, a terminating signal (SIGTERM, SIGINT,
SIGHUP or SIGALRM, which end the rank with status 128 plus the signal, 143
for SIGTERM), a fault, a runtime error, or news that a peer has ended.  The
moment a thread takes it is the rank's **shutdown point**.  From it the rank
sends nothing but, when its end began on this rank (not from a peer's message
or closed connection), one best-effort shutdown message to each peer it has
contacted; it writes its counter files (see :doc:`counters_cfg`) and ends the
process.  It does not wait for another rank, for its own tasks or for the
sends in flight; a peer learns of the end from the message or from the
closed connections.  The status of a rank is the highest of the end's own
status, that of every later entry (a thread that reaches the end after another
took it) and the one ``arts_app_exit_status`` returns.  A fault in a later
entry prints its report and stops its thread: it adds its status, 128 plus the
signal, but does not raise the signal, so it leaves no core.

An end can begin before the process knows its rank, in the configuration load
or name resolution: a signal, a configuration error or a ``resolve`` failure.
It takes the shutdown point all the same, so an end another thread begins
after it is a later entry, a fault in one included.  There is no peer to
tell and no counter file to write: the process arms the watchdog below,
flushes its output and ends with the highest of its own status and the later
entries'.  An end that began from a fault raises the fault's signal.

A call to ``abort`` (a failed ``assert``, a C library's own check, or an
allocator error, see :ref:`allocator_errors`) and a stack overflow take no
end: the process dies by the signal's default action, with status 134
(``SIGABRT``) or 139 (``SIGSEGV``, since the fault handler has no stack of its
own to run on), writes no counter files and sends no shutdown message; its
peers learn of the end from the closed connections.

A call to ``arts_shutdown`` does not return to its caller, and neither does
a call made after another thread took the end: the code after the call does
not run, and the calling EDT does not complete (its data blocks are not
released, its completion event is not satisfied, its group is not
decremented).

Every step after the shutdown point is bounded by a **watchdog** inside the
rank.  When the bound passes it prints
``[ARTS] shutdown watchdog: rank <r> stuck in phase <p>`` (the spawner's line
reads ``spawner`` in place of ``rank <r>``), where the phase is one of
``post``, ``capture``, ``regpool``, ``record``, ``oracle`` (only in a tree
built with ``ARTS_DEBUG_TEARDOWN``), ``leak`` (only under AddressSanitizer) or
``flush``, and ends the process: a rank whose end began from a fault raises
the fault's signal; a rank stuck in phase ``record`` ends with its own status,
0 included, the line adds ``; the counter record is lost``, and the counter
files are lost; any other rank ends with its status, or with 71 when that is
0.  The bound is 10 s, or 60 s in a tree built with a sanitizer or
``ARTS_DEBUG_TEARDOWN``, plus the counter files' estimated size at 4 MiB/s,
and the line is written only if standard error takes it within one more
second.  The watchdog and the startup timer below use the real-time signal
``SIGRTMIN+1``, which a program must not use and which the runtime unblocks
when ``arts_rt`` starts.  Neither can fire in a thread that is in
uninterruptible sleep (a write to a hung file system) or, under
ThreadSanitizer, in a thread outside a blocking call until its next one; the
spawner's grace bounds those.  A process the program forked, and a call to
the end from outside ``arts_rt``, have no watchdog.

Startup has a bound of its own.  The **startup timer**, told apart from the
watchdog by the value its signal carries, fires a second after the startup
budget passes (see the bootstrap mesh above) for a process stuck in a step
that cannot wait on the deadline itself: the launcher's query, name
resolution, thread placement, the fabric's bring-up, the pool's registration,
the address table's insertion or the fabric's self-check.  It writes the
``ARTS_MESH_FAILED`` line naming that step, if standard error takes it within
a second, and ends the process with status 75, writing no counter files; it
does nothing once the process's end began.  The bound ends on a rank of a run
of several once its mesh and its address exchange are complete; the spawner
ends it before it starts the ranks, and a run of one rank once its
configuration is loaded and checked, so the steps after those points do not
run under it.

The spawner gives the ranks of a run a **grace**: when the first rank has
ended, any rank still running after the grace is killed with SIGKILL, and the
spawner prints
``[ARTS] spawner: end-of-step grace of <G> s expired; killing rank <i>
(pid <p>)``.  The grace is the ``ARTS_END_GRACE_S`` environment variable, a
whole number of seconds in 1-86400.  Unset, it is 30 s, raised to the run's
startup budget plus the watchdog's bound when that is larger.  A set value
below that sum is refused at start, because it would kill a rank that was
about to end on its own.  A run that writes large counter files (``PERIODIC``
histories) sets it above the sum by the files' write time.  ``artsrun``
sets it per campaign with ``--end-grace`` and raises it for a cell whose bound
is larger (``tools/artsrun/README.md``, "End grace").

CXL Store
---------

Read only by a build whose own library is CXL-enabled (``ARTS_USE_CXL=ON``,
which implies ``EXCL`` × ``PURGE``); every other build parses and ignores
these keys.  The arena a rank allocates from is sized at build time
(``ARTS_CXL_DB_ARENA_SIZE_BYTES``), not by a config key.

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``cxl_db_allocation_strategy``
     - ``static``
     - Which arena a data block's payload allocates from — ``static`` (all
       allocations use the arena at ``cxl_db_allocation_device``'s index) or
       ``round_robin`` (allocations cycle across every arena the arena table
       spans). Arena creation does not honour device ids today, so both
       keys select an arena index, not a device.
   * - ``cxl_db_allocation_device``
     - 0
     - Arena index used under ``cxl_db_allocation_strategy=static``.

Debug / Utility
---------------

.. list-table::
   :header-rows: 1
   :widths: 25 12 63

   * - Key
     - Default
     - Description
   * - ``core_dump``
     - 0
     - 1 forces core dumps on crash; 0 lowers the soft core limit to zero, so
       a crash leaves no core file in the working directory.
   * - ``counter_folder``
     - ``./counters``
     - Output directory for performance counter files.
   * - ``counter_capture_interval``
     - 100
     - Capture interval for ``PERIODIC`` counters (milliseconds).
