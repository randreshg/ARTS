# The ThreadSanitizer residue of the data-block payload boundary.
#
# The runtime orders every copy of a block's payload it makes itself.  The
# fabric provider's own copies are what an EDT the program leaves unordered
# can overlap: the program's race, whose read the memory model leaves
# unspecified, not the runtime's.  A test reaches that residue through the
# shape of its acquisitions on an arm:
#   R1  a read is served from a buffer that a writer on the serving rank, not
#       ordered with the reader, is writing (the owner under write-back, the
#       home holding the write grant under write-through);
#   R2  a release publishes its write-through payload while another writer of
#       the same rank, not ordered with it, writes the block (a write turn
#       belongs to a rank, not to one EDT);
#   R3  a remote write-through release lands in the home's buffer in place
#       while a reader on the home, not ordered with it, reads it;
#   R4  a remote acquisition fetches the home's line while a writer on the
#       home, not ordered with it, writes it (FLUSH);
#   R5  a remote write-back lands in the home's line in place while a reader
#       on the home, not ordered with it, reads it (FLUSH).
# An exclusion arm (EXCL, the CXL store) orders every copy by its admission
# and has no residue; a single-rank run brings no fabric up.
#
# One entry per (test, cells): the cells whose arm reaches a shape in that
# test's program, the shapes, and the multinode variants that reach them
# (every multinode variant the test registers when VARIANTS is absent).  A
# ThreadSanitizer tree of one of those cells runs exactly those variants with
# the suppressions of the entry's shapes: the provider's send-side copies for
# R1, R2 and R4, its receive-side copies as well only for an entry that lists
# R3 or R5.  Every other test keeps the provider's frames visible, so a
# provider-frame report there fails it.  A shape stays listed when a run
# happens not to reach it: whether it does depends on timing.  A shape must
# be one the cell's arm can reach (R1 on every arm but FLUSH, R2 and R3 under
# write-through only, R4 and R5 under FLUSH only).
#
# Cells: VAL/WB/RETAIN, INV/WB/RETAIN, VAL/WT/RETAIN, VAL/WT/PURGE,
# INV/WT/RETAIN, INV/WT/PURGE, FLUSH.

# RO readers on a rotating window of ranks, unordered with the round's
# writers on every rank, several writers per rank, homes round-robin.
arts_tsan_residue(coherence_writer_ro_overlap
    CELLS VAL/WB/RETAIN INV/WB/RETAIN SHAPES R1)
arts_tsan_residue(coherence_writer_ro_overlap
    CELLS VAL/WT/RETAIN VAL/WT/PURGE INV/WT/RETAIN INV/WT/PURGE
    SHAPES R1 R2 R3)

# An unordered reader on another rank against the creator, which writes the
# block through its create hold on the home.
arts_tsan_residue(db_create_claims_first_touch_stub
    CELLS VAL/WB/RETAIN INV/WB/RETAIN VAL/WT/RETAIN VAL/WT/PURGE
          INV/WT/RETAIN INV/WT/PURGE
    SHAPES R1)
arts_tsan_residue(db_create_claims_first_touch_stub
    CELLS FLUSH SHAPES R4)

# One writer and one reader per rank in each generation, unordered; the
# readers read nothing, so only the serve of a written buffer races.
arts_tsan_residue(cat_c_ref_balance
    CELLS VAL/WB/RETAIN INV/WB/RETAIN VAL/WT/RETAIN VAL/WT/PURGE
          INV/WT/RETAIN INV/WT/PURGE
    SHAPES R1)

# The first writer, off the home, against unordered readers: served by the
# writer's rank under write-back on every variant; on the home itself only
# with two ranks, where the write-through publish and the FLUSH write-back
# land in the buffer they read.
arts_tsan_residue(db_no_acquire_wb_first_writer
    CELLS VAL/WB/RETAIN INV/WB/RETAIN SHAPES R1)
arts_tsan_residue(db_no_acquire_wb_first_writer
    CELLS VAL/WT/RETAIN VAL/WT/PURGE INV/WT/RETAIN INV/WT/PURGE
    SHAPES R3 VARIANTS 2n_9w_1p)
arts_tsan_residue(db_no_acquire_wb_first_writer
    CELLS FLUSH SHAPES R5 VARIANTS 2n_9w_1p)

# Writers (atomic increments, several per rank) and readers that read
# nothing, unordered, on every rank.
arts_tsan_residue(excl_multiconsumer_queue
    CELLS VAL/WB/RETAIN INV/WB/RETAIN SHAPES R1)
arts_tsan_residue(excl_multiconsumer_queue
    CELLS VAL/WT/RETAIN VAL/WT/PURGE INV/WT/RETAIN INV/WT/PURGE
    SHAPES R1 R2)

# Several unordered writers of one block on a rank that is not its home; a
# reader only after all of them.
foreach(_t IN ITEMS
        coherence_multi_writer_dist
        coherence_multi_writer_multi_db
        coherence_owner_ownership_reorder
        grant_purge_churn
        grant_purge_handback_regrant
        grant_one_request_per_grant
        quiescent_shutdown_no_residue
        await_publish_ack_shutdown_contended)
    arts_tsan_residue(${_t}
        CELLS VAL/WT/RETAIN VAL/WT/PURGE INV/WT/RETAIN INV/WT/PURGE
        SHAPES R2)
endforeach()

# The create hold, which writes nothing, against the ordered writer on the
# same rank, which it does not order: the hold's release publishes while that
# writer writes.
arts_tsan_residue(db_create_after_dep_asked
    CELLS VAL/WT/RETAIN VAL/WT/PURGE INV/WT/RETAIN INV/WT/PURGE
    SHAPES R2)

# The frontier's one writer per step, on its home, against the RO instances on
# other ranks, which never read it.
arts_tsan_residue(random_access_example_smoke
    CELLS VAL/WB/RETAIN INV/WB/RETAIN VAL/WT/RETAIN VAL/WT/PURGE
          INV/WT/RETAIN INV/WT/PURGE
    SHAPES R1)
arts_tsan_residue(random_access_example_smoke
    CELLS FLUSH SHAPES R4)

# The tile's home writes the force caches and the gradient blocks' quantities
# while the vertex-corner reduction and the face neighbours read other words
# of those blocks, unordered with the write.
arts_tsan_residue(lulesh_example
    CELLS VAL/WB/RETAIN INV/WB/RETAIN VAL/WT/RETAIN VAL/WT/PURGE
          INV/WT/RETAIN INV/WT/PURGE
    SHAPES R1)
arts_tsan_residue(lulesh_example
    CELLS FLUSH SHAPES R4)
