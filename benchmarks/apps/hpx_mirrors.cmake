# The HPX applications section's OCR mirrors (ARTS_BUILD_HPX_APPS): the
# OCR-API programs under hpx_origin/ that mirror the HPX originals, registered
# through the factories of CMakeLists.txt, which includes this file.
add_bespoke_app(NAME fib_hpx DIR hpx_origin SOURCES fib_hpx.c
    EXTRA_LINK_LIBS arts_fib_kernel)
add_bespoke_app(NAME stencil1d_hpx DIR hpx_origin SOURCES stencil1d_hpx.c)
add_bespoke_app(NAME network_storage_hpx DIR hpx_origin SOURCES network_storage_hpx.c)
# Retained library pointers use acquired object identities across tasks.
# The vendored FFTW is configured with this section, and the mirror links the
# relocation library, a pinned translation of the same FFTW source that the
# tree's own compiler assembles (hpx_origin/fftw_reloc/), so the row asks
# nothing more of the host than the vendored FFTW does.
add_subdirectory(hpx_origin/fftw_reloc)
add_bespoke_app(NAME fft_hpx DIR hpx_origin SOURCES fft_hpx.c
    EXTRA_LINK_LIBS arts::fftw_reloc m)
