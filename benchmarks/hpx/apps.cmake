# The HPX-origin programs, one name each.  A name here is a directory
# benchmarks/hpx/<name>/ with an ORIGIN.md, an origin.patch and the sources,
# an executable <name>_hpx, a catalog row of the same name with an OCR
# mirror binary <name>, and an entry in the outer project's byproduct list —
# every one of those follows from this list, so a program is added here and
# nowhere else.
set(ARTS_HPX_APPS fib_hpx pi_hpx random_mem_access_hpx stencil1d_hpx transpose_hpx
    jacobi_hpx network_storage_hpx mini_ghost_hpx nbody_hpx fft_hpx sheneos_hpx)
