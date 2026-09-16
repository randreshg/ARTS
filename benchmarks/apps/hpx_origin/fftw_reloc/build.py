#!/usr/bin/env python3
"""Build the typed retained-state representation from the pinned FFTW source."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tarfile


PROBE = """#include "kernel/ifftw.h"
#include "simd-support/simd-common.h"
#include <stdio.h>
int main(void)
{
#if HAVE_SIMD
    printf("%d %d %d\\n", (int)ALIGNMENT, (int)ALIGNMENTA, 16);
#else
    printf("0 0 0\\n");
#endif
    return 0;
}
"""


def simd_alignment(clang, source, build, run):
    """Read the alignment the configured library's own predicates apply.

    ALIGNMENT and ALIGNMENTA bound every address mask the SIMD codelet
    machinery tests, and the alignment reported for a pointer is taken modulo
    the same unit, so their maximum is the widest mask the representation has
    to evaluate relative to a logical allocation base.
    """
    probe = build / "alignment-probe.c"
    probe.write_text(PROBE)
    run([clang, "-I" + str(build), "-I" + str(source), "-o",
         str(build / "alignment-probe"), str(probe)])
    reported = subprocess.run([str(build / "alignment-probe")], cwd=build,
                              check=True, capture_output=True, text=True)
    alignment = max(int(field) for field in reported.stdout.split())
    if alignment < 16 or alignment & (alignment - 1):
        raise RuntimeError(
            "the representation normalises alignment predicates and needs a "
            "SIMD configuration whose alignment is a power of two of at least "
            "16 bytes; this configuration reports " + reported.stdout.strip())
    print("simd alignment " + str(alignment), flush=True)
    return alignment


def main():
    parser = argparse.ArgumentParser()
    for name in ("archive", "work", "clang", "link", "translator"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--simd", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    archive = Path(args.archive).resolve()
    expected = "56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467"
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise RuntimeError("FFTW source archive checksum mismatch")
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = work / "fftw-3.3.10"
    if not source.exists():
        with tarfile.open(archive) as package:
            package.extractall(work)
    build = work / "bitcode"
    build.mkdir(exist_ok=True)

    def run(command):
        print(" ".join(str(item) for item in command), flush=True)
        subprocess.run(command, cwd=build, check=True)

    flags = "-O1 -g -march=native -mtune=native"
    run([str(source / "configure"), "--enable-static", "--disable-shared",
         "--disable-fortran", "--disable-mpi", "--disable-threads",
         "--disable-openmp", "--with-pic", *args.simd,
         "CC=" + args.clang, "CFLAGS=" + flags])
    alignment = simd_alignment(args.clang, source, build, run)
    for part in ("kernel", "simd-support", "dft", "rdft", "reodft", "api"):
        run(["make", "-C", part, "-j" + str(args.jobs), "all",
             "CFLAGS=" + flags + " -flto"])
    objects = sorted(build.rglob("*.o"))
    if not objects:
        raise RuntimeError("no FFTW bitcode objects")
    response = work / "objects.rsp"
    response.write_text("\n".join('"' + str(path) + '"' for path in objects) + "\n")
    run([args.link, "@" + str(response), "-o", str(work / "fftw-stock.bc")])
    run([args.translator, str(work / "fftw-stock.bc"), str(work / "fftw-reloc.bc"),
         str(alignment)])
    run([args.clang, "-O3", "-fPIC", "-c", str(work / "fftw-reloc.bc"),
         "-o", str(work / "fftw-reloc.o")])


if __name__ == "__main__":
    main()
