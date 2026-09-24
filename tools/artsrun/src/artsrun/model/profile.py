"""Machine/node settings: how many nodes, how wide each one runs, how the
ranks are launched."""

from __future__ import annotations

from enum import StrEnum

from pydantic import BaseModel, ConfigDict, Field, model_validator


class Launcher(StrEnum):
    LOCAL = "local"
    SSH = "ssh"
    SLURM = "slurm"
    FLUX = "flux"


class FamDevice(StrEnum):
    """Whether, and on which device library, the fabric-attached-memory
    entries run.

    `off`: fabric-attached memory is not available where this profile runs
    (ARTS_FAM_BACKEND=OFF).  `fake`: the vendored fake library, one host's
    shared memory standing in for the device (ARTS_FAM_BACKEND=SHM).
    `real`: the device library itself, named by path (ARTS_FAM_BACKEND=DEVICE).
    """

    OFF = "off"
    FAKE = "fake"
    REAL = "real"


# A typoed profile key must refuse, not vanish: some keys (slurm.mpi in
# particular) are the only handle against failures that are otherwise
# silent, so a key that "took" while doing nothing is the worst outcome.
_STRICT = ConfigDict(extra="forbid")


class SshSettings(BaseModel):
    """The nodes this campaign may use, and their names.

    `budget` states how many nodes are available; `hosts` must name exactly
    that many, in rank order.  Stating the count separately is what catches a
    roster that quietly lost a line.
    """

    model_config = _STRICT

    budget: int = Field(ge=1)
    hosts: list[str] = Field(default_factory=list)


class SlurmSettings(BaseModel):
    """Submission parameters.

    Each cell is its own exclusive job — scheduling the queue is Slurm's whole
    purpose, so no node budget of ours meters it, and every job records its
    own outcome, so what is already queued survives the submitting login node.
    """

    model_config = _STRICT

    partition: str | None = None
    # Build work may queue elsewhere than the cells: a debug partition takes
    # a small compile job sooner than the batch partition takes a node.
    build_partition: str | None = None
    # A compile is not a measurement — it asks for a handful of cpus it can
    # get anywhere in the queue, never an exclusive node.
    build_cpus: int = Field(default=8, ge=1)
    account: str | None = None
    qos: str | None = None
    # The most jobs of this campaign that sit in the queue, pending or
    # running, at one time.  A site caps the jobs one user may have submitted,
    # and a campaign of several thousand cells crosses it; the rest are
    # submitted as these finish.  Unset submits everything up front.  With a
    # cap the submitter has work left until the last cell is out, so a
    # submitter that dies is continued with --resume rather than merely
    # watched.
    max_queued: int | None = Field(default=None, ge=1)
    # srun's --mpi plugin for the reference cells (pmi2/pmix).  Unset relies
    # on the site's MpiDefault; set it when the site default cannot form the
    # MPI world — the failure that produces is otherwise SILENT (each rank
    # degrades to a size-1 singleton world and "succeeds" alone).
    mpi: str | None = None
    extra_sbatch: list[str] = Field(default_factory=list)
    poll_interval_s: float = Field(default=10.0, gt=0)


class FluxSettings(BaseModel):
    """Submission parameters for a Flux cluster.

    Same contract as Slurm: each cell is its own exclusive job, submitted up
    front, writing its own outcome marker — the queue is the scheduler's to
    run and the submitter is optional.
    """

    model_config = _STRICT

    queue: str | None = None
    # Build work may queue elsewhere than the cells — but a debug queue's
    # short time cap kills a full build mid-link, so this defaults to the
    # cell queue, not to debug.
    build_queue: str | None = None
    build_cpus: int = Field(default=8, ge=1)
    # Rendered as -t on the build job; unset inherits the queue default.
    build_time: str | None = None
    # The accounting bank the node-hours are charged to; rendered --bank=.
    # (--setattr=system.bank= is the fallback spelling on an instance whose
    # flux-core predates the option.)
    bank: str | None = None
    # Shell PMI service list for the reference cells (-o pmi=...).  Unset
    # relies on the site default; set it when that default cannot form the
    # MPI world — the failure that produces is otherwise SILENT (each rank
    # degrades to a size-1 singleton world and "succeeds" alone).
    pmi: str | None = None
    # True = the site loads the mpibind plugin, so every run line disables
    # it (-o mpibind=off).  False omits the option entirely: an instance
    # without the plugin may reject the unknown name.
    mpibind: bool = True
    extra_batch: list[str] = Field(default_factory=list)
    extra_run: list[str] = Field(default_factory=list)
    poll_interval_s: float = Field(default=10.0, gt=0)


class Profile(BaseModel):
    model_config = _STRICT

    name: str
    launcher: Launcher
    nodes: list[int] = Field(min_length=1)
    workers: int = Field(ge=1)
    progress: int = Field(ge=0)

    pin: bool = True
    provider: str | None = None
    route_table_size: int = 16
    core_dump: bool = False
    net_interface: str | None = None
    fabric_domain: str | None = None
    regpool_slab_mb: int | None = None

    # The fabric-attached-memory pool's size: a cfg key a fam-enabled binary
    # reads and any other simply never looks up.  Unset leaves the runtime's
    # own default in force.
    fam_pool_mb: int | None = Field(default=None, ge=1)
    # Strict mode: a private copy-on-write view of the pool per rank over a
    # shared backing, so a byte reaches a peer only once its producer flushed
    # it and the consumer reloaded it.  On one host every rank shares one
    # coherent cache, so without it a missing flush can never show as a wrong
    # value; a store that is not coherent across hosts has that second
    # domain already.  So it is a setting of fam_device: fake alone, on
    # unless stated otherwise there, and an error beside real or off.  A
    # test oracle, never a measurement mode.
    fam_strict: bool | None = None

    # The device library the fabric-attached-memory entries run on.  Named
    # here and nowhere else: the build tree is made to match it or the
    # campaign does not start, so which library a FAM cell ran on is always
    # the one its profile states.  Absent means off, except under
    # launcher=local, which takes `fake` (one host, no fabric: the vendored
    # library is the only one that applies there).
    fam_device: FamDevice | None = None
    # The device library's headers and the library file; `real` only.
    fam_device_include_dir: str | None = None
    fam_device_library: str | None = None

    # Worker stack, in MiB, given to EVERY runtime a campaign measures — the
    # runtime under test and the references alike.  A runtime whose message
    # handling recurses on the worker stack has a multinode depth bounded by
    # this value, so leaving it at the platform default (8 MiB, from
    # RLIMIT_STACK) does not measure the runtime, it measures the default.
    # Applying one value to all of them is what keeps a difference between
    # them a difference in the runtimes.  Stacks are reserved, not committed,
    # so the cost is address space until a recursion actually descends.
    # 8 MiB is what both runtimes fall back to on their own -- ARTS skips the
    # attribute when the key is zero, the reference substitutes 8388608 -- and
    # it is the platform's own thread default.  Stating it rather than leaving
    # it implicit keeps the two measured at the same size; raise it only for a
    # runtime whose depth the stack actually bounds, and say why in the profile.
    stack_size_mb: int = Field(default=8, ge=0)

    # Parallel connections each node opens.  A local run has the runtime find
    # its own block of node_count x port_count; a remote one must be told which
    # ports to use, and then the list has to name exactly this many.
    port_count: int = Field(default=1, ge=1)
    ports: list[int] = Field(default_factory=list)

    ssh: SshSettings | None = None
    slurm: SlurmSettings | None = None
    flux: FluxSettings | None = None

    cell_timeout_s: int = Field(default=300, ge=1)
    repeats: int = Field(default=1, ge=1)

    # Wrap every cell's binary in a getrusage witness (`/usr/bin/time -v`) to
    # recover peak resident set size and minor-fault counts alongside the
    # runtime's own end-to-end stamp — neither is otherwise observed. Off by
    # default: the wrapper is a per-rank extra process, and a site whose
    # compute nodes lack the binary simply runs without it (guarded at
    # launch, not here) rather than refusing the flag.
    rusage_witness: bool = False

    @property
    def threads_per_node(self) -> int:
        """The core block one rank occupies.

        Whether the machine actually has that many is left to the run: ARTS
        does not oversubscribe, and a geometry that asks for more threads than
        there are cores fails at startup on its own.
        """
        return self.workers + self.progress

    @property
    def max_nodes(self) -> int:
        return max(self.nodes)

    @property
    def hosts(self) -> list[str]:
        return self.ssh.hosts if self.ssh else []

    @property
    def sched_settings(self) -> SlurmSettings | FluxSettings | None:
        """The scheduler section this profile's launcher reads.

        The build-job width and the poll cadence mean the same thing under
        either scheduler; going through one accessor keeps every consumer
        from wiring itself to a single launcher's section.
        """
        if self.launcher is Launcher.SLURM:
            return self.slurm
        if self.launcher is Launcher.FLUX:
            return self.flux
        return None

    @property
    def resolved_fam_device(self) -> FamDevice:
        """What a FAM entry run under this profile links: the stated value;
        absent, `fake` under launcher=local (one host, no fabric: the vendored
        library is the only one that applies there) and `off` elsewhere."""
        if self.fam_device is not None:
            return self.fam_device
        if self.launcher is Launcher.LOCAL:
            return FamDevice.FAKE
        return FamDevice.OFF

    @property
    def resolved_fam_strict(self) -> bool | None:
        """The fam_strict value the rendered cfg carries: the stated one, on
        by default under fam_device: fake, and none at all elsewhere."""
        if self.resolved_fam_device is not FamDevice.FAKE:
            return None
        return True if self.fam_strict is None else self.fam_strict

    def _check_fam_statement(self) -> None:
        """Refuse a device-library statement that is wrong whatever runs."""
        device = self.resolved_fam_device
        paths = [k for k in ("fam_device_include_dir", "fam_device_library")
                 if getattr(self, k)]
        if device is not FamDevice.REAL and paths:
            raise ValueError(
                f"profile '{self.name}': {' and '.join(paths)} belong to "
                f"fam_device: real, and this profile's fam_device is {device}")
        if self.launcher is Launcher.LOCAL and device is FamDevice.REAL:
            raise ValueError(
                "fam_device: real is not allowed under launcher=local: one "
                "host has no fabric, so the vendored library is the only one "
                "that applies (local takes off or fake)")
        if device is FamDevice.REAL:
            missing = [k for k in ("fam_device_include_dir", "fam_device_library")
                       if not getattr(self, k)]
            if missing:
                raise ValueError(
                    f"fam_device: real requires {' and '.join(missing)}: the "
                    "device library's headers and the library file")
            relative = [k for k in paths if not getattr(self, k).startswith("/")]
            if relative:
                raise ValueError(
                    f"{' and '.join(relative)} must be absolute paths: the "
                    "build tree is configured from them wherever the build runs")
        if (device is FamDevice.FAKE and self.launcher is not Launcher.LOCAL
                and self.nodes != [1]):
            raise ValueError(
                f"fam_device: fake under launcher={self.launcher} requires "
                f"nodes: [1] (got {self.nodes}): the vendored library is one "
                "host's shared memory, so its pool reaches only ranks that "
                "share a host")
        if self.fam_strict is not None and device is not FamDevice.FAKE:
            raise ValueError(
                f"fam_strict is a setting of fam_device: fake only, and this "
                f"profile's fam_device is {device}: strict mode gives one "
                "host's memory the second coherency domain a store that is "
                "not coherent across hosts has, so it means nothing over the "
                "device library (real) or with no fabric-attached memory "
                "(off); remove fam_strict")

    def check_fam_device(self, fam_entries: list[str]) -> None:
        """Refuse a campaign whose fabric-attached-memory entries this
        profile's device library cannot run."""
        if fam_entries and self.resolved_fam_device is FamDevice.OFF:
            raise ValueError(
                f"profile '{self.name}' has fam_device: off (absent means off "
                "except under launcher=local: fabric-attached memory is not "
                "available where it runs), so it cannot run "
                f"{', '.join(fam_entries)}; select a profile whose fam_device "
                "is fake or real, or leave these entries out")

    @model_validator(mode="before")
    @classmethod
    def _no_entries(cls, data):
        """The entry list moved to the experiment: a node profile states
        host facts, and which coherence entries run is the study's choice."""
        if isinstance(data, dict) and "entries" in data:
            raise ValueError(
                "a node profile no longer lists entries: the coherence entries "
                "a campaign runs by default moved to the experiment "
                "(experiments/experiments/<name>.yaml, `entries:`), and -e "
                "names others for one run; remove `entries` from this profile")
        return data

    @model_validator(mode="after")
    def _check(self) -> "Profile":
        if self.launcher is Launcher.LOCAL:
            if self.ports:
                raise ValueError(
                    "ports must not be set for launcher=local: the ranks share a "
                    "machine, so the spawning rank finds a free block itself"
                )
        else:
            if not self.ports:
                raise ValueError(f"ports is required for launcher={self.launcher}")
            if len(self.ports) != self.port_count:
                raise ValueError(
                    f"ports names {len(self.ports)} port(s) but port_count is "
                    f"{self.port_count}; the list must name exactly that many"
                )
        if self.launcher is Launcher.SSH:
            if self.ssh is None:
                raise ValueError("launcher=ssh requires an ssh section")
            if len(self.ssh.hosts) != self.ssh.budget:
                raise ValueError(
                    f"ssh.hosts names {len(self.ssh.hosts)} host(s) but "
                    f"ssh.budget is {self.ssh.budget}; name one host per node"
                )
            if self.ssh.budget < self.max_nodes:
                raise ValueError(
                    f"ssh.budget={self.ssh.budget} is below the widest node "
                    f"count ({self.max_nodes}); that cell has nowhere to run"
                )
            if len(set(self.ssh.hosts)) != len(self.ssh.hosts):
                # Remote hosts are distinct physical servers by contract —
                # colocating ranks on one machine is what launcher=local is
                # for, and two remote ranks on one host would claim the same
                # CPU envelope.
                raise ValueError("ssh.hosts must name distinct hosts")
        if self.launcher is Launcher.SLURM and self.slurm is None:
            raise ValueError("launcher=slurm requires a slurm section")
        if self.launcher is Launcher.FLUX and self.flux is None:
            raise ValueError("launcher=flux requires a flux section")
        if self.provider == "verbs":
            # The runtime requires RDM endpoints; the verbs core provider
            # offers only connection-oriented MSG endpoints, so RDM exists
            # solely as the layered stack — and fi_getinfo treats a lone core
            # name as excluding utility layering, so bare "verbs" can never
            # match.  The layered name is the unit, stated explicitly so its
            # presence is a user decision rather than a silent rewrite.
            raise ValueError(
                "provider=verbs can never match: name the layered stack "
                "explicitly — provider=verbs;ofi_rxm"
            )
        if any(n < 1 for n in self.nodes):
            raise ValueError("node counts must be >= 1")
        self._check_fam_statement()
        return self
