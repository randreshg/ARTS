"""Machine/node settings: how many nodes, how wide each one runs, how the
ranks are launched."""

from __future__ import annotations

from enum import StrEnum
from typing import ClassVar

from pydantic import BaseModel, ConfigDict, Field, model_validator


class Launcher(StrEnum):
    LOCAL = "local"
    SSH = "ssh"
    SLURM = "slurm"
    FLUX = "flux"


class CxlLibrary(StrEnum):
    """Which library the CXL entries link: the vendored fake (one host's
    shared memory standing in for the device) or the device's own, named by
    the profile's three paths."""

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

    # The device library the CXL entries link, and the site's launch wrapper
    # that sets the device's regions up around a whole launch.  All three or
    # none: none is the vendored fake library, which the tree builds itself;
    # a subset is a mistake, refused here.  Absolute paths: the build tree is
    # configured from them wherever the build runs.
    cxl_include_dir: str | None = None
    cxl_library: str | None = None
    cxl_launch_wrapper: str | None = None

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

    CXL_PATHS: ClassVar[tuple[str, ...]] = ("cxl_include_dir", "cxl_library", "cxl_launch_wrapper")

    @property
    def cxl_library_kind(self) -> CxlLibrary:
        return CxlLibrary.REAL if self.cxl_library else CxlLibrary.FAKE

    def _check_cxl_statement(self) -> None:
        """All three paths or none, and absolute."""
        given = [k for k in self.CXL_PATHS if getattr(self, k)]
        if given and len(given) != len(self.CXL_PATHS):
            missing = [k for k in self.CXL_PATHS if k not in given]
            raise ValueError(
                f"profile '{self.name}': the CXL device library takes all of "
                f"{', '.join(self.CXL_PATHS)}; {' and '.join(missing)} missing "
                "(leave all three out for the vendored fake library)")
        relative = [k for k in given if not getattr(self, k).startswith("/")]
        if relative:
            raise ValueError(
                f"{' and '.join(relative)} must be absolute paths: the build "
                "tree is configured from them wherever the build runs")

    def check_cxl(self, cxl_entries: list[str]) -> None:
        """Refuse a campaign whose CXL entries would run on the vendored fake
        across hosts.  Campaign-time, not load-time: a profile that names no
        device library is every other campaign's ordinary profile."""
        if (cxl_entries and self.cxl_library_kind is CxlLibrary.FAKE
                and self.launcher is not Launcher.LOCAL and self.nodes != [1]):
            raise ValueError(
                f"profile '{self.name}' names no CXL device library, so "
                f"{', '.join(cxl_entries)} would run on the vendored fake, which "
                f"is one host's shared memory: under launcher={self.launcher} "
                f"that needs nodes: [1] (got {self.nodes}); name the three cxl_* "
                "paths for the device, or leave these entries out")

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
        self._check_cxl_statement()
        return self
