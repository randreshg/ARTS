# FFTW retained state

This representation port builds FFTW 3.3.10 from its pinned source archive. It
keeps the planner, solver candidates, numerical codelets, and caller's planning
flags. A typed LLVM 14 translation supplies an explicit acquisition context to
internal calls and callbacks. The port is used by the OCR FFT application; the
native HPX application continues to link the ordinary FFTW archive.

## Storage and lifetime

Each planner allocation receives an identity. Persistent pointer fields contain
a 4-bit kind, 20-bit identity, and 40-bit byte offset. Immutable code and data use
deterministic module symbol identities; an image fingerprint rejects a different
module. The 13 mutable FFTW globals become objects in the explicit context.
Their original definitions are immutable initialization templates, with generated
pointer-field offsets for initialization. No process-global or thread-local
pointer map is used.

The two plans share one locality context. Planning operates on the first row of
each acquired numerical buffer, then a manifest and its live retained
allocations are published in one DB. The manifest is three arrays over that one
image: allocation entries keyed by identity, so resolving an identity is an
index; locators ordered by the image offset at which each retained allocation's
bytes sit, so finding the allocation that contains an address is a search; and
the mutable globals keyed by their generated index. An entry carries the
allocation's extent, its typed pointer-field list, and, for a borrowed
allocation, the identity that allocation has outside the image.

A whole numerical buffer is one borrowed object. The manifest records its
identity and extent and never its bytes, so a row inside it is addressed as that
identity plus a byte offset — the row pointers the plan itself retained are
exactly such offsets. New-array execution's original-row pointer differences are
computed on identity and offset, without reading an unacquired buffer.

Each row task acquires the image RO and the buffer its row lives in RW. Opening
the image for execution is a header check and four pointer computations over
those arrays plus one binding for the lent buffer: nothing is rebuilt per row and
no allocation is walked. Locating the allocation that contains an address is a
binary search over the image's own locator array, then the lent buffer, then
whatever scratch the transform allocated for itself. A retained allocation's
extent is checked where it is used rather than in a sweep at open time. The
pointer-field list is metadata for validation and publication, so an execution
context, which publishes nothing, does not carry it. Retained object bytes are
used directly; no native plan graph is materialized or copied. Execution cannot
mutate or free retained objects, and cannot free the lent buffer; any allocation
during execution is private scratch. The final task acquires the image RW after
both transform phases finish, destroys the r2c plan, destroys the c2c plan, and
calls cleanup. That sequence does write retained storage and needs the pointer
field lists, so it opens the image the writable way — once per locality, not per
row. Logical frees during it preserve object identities; the image DB is
reclaimed afterward.

## Alignment and compilation

Logical numerical buffer bases have the source allocator's 16-byte alignment;
retained allocator objects have 64-byte alignment. Acquired physical bases need
only 8-byte alignment. The translator evaluates alignment masks relative to the
logical allocation base and lowers aligned vector loads/stores, including inline
assembly moves, to unaligned-safe memory operations. It does not add
`FFTW_UNALIGNED`, move the row, or change numerical arithmetic.

The alignment those masks are recognised against is read from the configured
library, not assumed. `build.py` compiles a probe against the configured tree
and takes the largest of `ALIGNMENT`, `ALIGNMENTA` and the unit
`ialignment_of` reports; the translator treats a constant mask at or below that
value minus one as an alignment predicate and evaluates its address operands
relative to their allocation, and records the value it was given. Double
precision with `--enable-sse2 --enable-avx --enable-avx2 --enable-fma` gives 16,
which is what every recorded result was taken at. A configuration whose
alignment is not a power of two of at least 16 bytes fails the build with the
reason instead of leaving a predicate address-dependent: unnormalised, plan
selection would depend on the physical address a data block happened to receive,
per rank and per run.

A row's byte offset inside its buffer has to start on a complex element, which
holds for any buffer whose columns are whole complex elements. That is what
makes every row carry its buffer's alignment, and plan selection therefore
address-independent for every row rather than only the first.

The front end uses `-O1 -g -march=x86-64-v3 -flto` to retain typed pointer
operations; the level names what the configured SIMD sets need (AVX2 and FMA)
rather than the generating host, so the artifact runs on every x86-64 host the
experiments use and its code does not depend on where it was generated. The
translated module, its debug metadata stripped, is compiled to assembly with
`-O3 -fPIC -fno-addrsig -S`; the native HPX side's FFTW is compiled at `-O3`
and the same `-march` level, read from the artifact's stamp, so the two sides
differ in compiler and translation and not in the instruction set either was
allowed. SIMD and FMA configuration comes from `ARTS_FFTW_SIMD`. These compilation and
representation choices add costs: pointer resolution, callback context passing,
manifest publication, the per-row image attach and lent-buffer binding, and
unaligned-safe memory accesses. The identity-keyed and offset-ordered arrays
cost image size — five words per identity and three per retained allocation — in
exchange for an attach that does no per-allocation work. Measured planning can
select a different candidate because its measured costs include this
representation. This is not instruction-for-instruction equivalence or a promise
that timed planning selects identical plans.

## Reproduction and schema

The translated library is a pinned artifact of the source tree, committed as
text: `pinned/fftw-reloc.s` is the assembly `build.py` generated from the
pinned source, `pinned/fftw-reloc.bc.schema` the translation's record of the
module, and `pinned/fftw-reloc.stamp` the source checksum, the SIMD set, the
alignment, the `-march` level and the generating clang. An ordinary configure
checks the stamp against the tree's FFTW archive and `ARTS_FFTW_SIMD` (a
mismatch is a configure error naming the regeneration), and the library is
assembled and linked by the compiler the tree was configured with — GCC or
clang, whichever `project()` detected. Nothing in an ordinary build looks for
LLVM.

Regeneration is opt-in: `-DFFTW_RELOC_REGENERATE=ON` and the `fftw_reloc_pin`
target. That configuration locates LLVM 14 in the existing environment or the
repository's `install/fftw-port-tools` directory — LLVM 14 is the version, not
a floor, because the translation works on typed pointer operations, which
later LLVM releases no longer carry — and rewrites the pinned files in the
source tree, so the artifact a tree builds from is always the one it carries,
and a regeneration's effect on the code is a `git diff` of the assembly.
`build.py` checks the source SHA-256, extracts
it in the build tree, builds all configured source modules, links their bitcode,
reads the configured alignment, runs the translator, emits the output as
assembly, assembles it once with the generating host's C compiler so a dialect
the GNU assembler rejects fails at generation rather than at consumption, and
places the artifact's files in `pinned/`. It never changes the source archive
or installs system tools.

`fftw-reloc.bc.schema` records the data layout, all 88 structure layouts and
pointer offsets, 2,674 function pointer-access/callback counts, 250 integer/address
conversion sites, 45 bulk-copy sites, and constant/context global classifications
for the configured x86 SIMD build. Other source/configuration combinations may
have different counts. Aggregate pointer loads/stores that require scalarization
are rejected rather than silently copied.

`fftw_reloc_validate` checks the dynamic typed-field closure before publication,
including raw-pointer and dangling-object rejection. `fftw_reloc_dump` writes
every live allocation's extent, ownership and alignment, followed by every
retained pointer field and its compiled symbol name when applicable.
`FFTW_RELOC_AUDIT` additionally counts allocations, frees, pointer loads and
stores, and sort calls and callbacks, which `fftw_reloc_trace` prints with the
live object and pointer-field census.

`FFTW_RELOC_PROTECT` is a second diagnostic: the image's own pages become
read-only to the hardware for the span of the transforms, so a store into
retained storage traps where it happens instead of being argued about. It needs
the image page-aligned, and an acquired payload is only 64-byte aligned, so
under this option `fftw_reloc_image_reserve` asks for one page more than the
image and `fftw_reloc_image_at` places the image at the first page boundary
inside the block; both are the identity otherwise, so a caller uses them
unconditionally and the ordinary build reserves and places nothing extra. The
protection covers the application's own stores only in the sense that it covers
every store to those pages: a run whose coherence arm writes that payload from
outside the application — a write-through publication, or a fetch landing in the
same buffer — traps on the runtime's own copy. The option therefore belongs to a
single-locality diagnostic run, never to a measurement.

These diagnostics are separate from ordinary application execution.
