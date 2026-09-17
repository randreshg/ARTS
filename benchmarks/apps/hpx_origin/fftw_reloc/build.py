#!/usr/bin/env python3
"""Build the typed retained-state representation from the pinned FFTW source.

The output is assembly: the translation is an LLVM IR transformation and
needs LLVM 14 to run, but its result is committed alongside the translator
as a pinned artifact, so a tree builds the library with the compiler it was
configured with and needs LLVM only to regenerate the artifact.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
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

FFTW_SHA256 = "56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467"

# What the pinned artifact carries besides the assembly: the translation's
# record of the module, and the stamp a tree checks its configuration against.
ARTIFACT = ("fftw-reloc.s", "fftw-reloc.bc.schema", "fftw-reloc.stamp")


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


def tool_version(tool):
    out = subprocess.run([tool, "--version"], check=True, capture_output=True,
                         text=True).stdout
    return out.splitlines()[0].strip()


def main():
    parser = argparse.ArgumentParser()
    for name in ("archive", "work", "clang", "link", "translator"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--simd", action="append", default=[])
    # The instruction set the artifact is generated for.  The configured SIMD
    # codelet sets fix what it needs; naming the level rather than the
    # generating host keeps the artifact runnable on every host the
    # experiments use and its code the same wherever it was generated.
    parser.add_argument("--march", default="x86-64-v3")
    # The assembler the artifact is meant for: the generating host's C
    # compiler assembles the output once, so an assembly dialect the GNU
    # assembler does not take is caught here and not on the consuming host.
    parser.add_argument("--cc")
    parser.add_argument("--pin", help="directory to place the artifact's files in")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    archive = Path(args.archive).resolve()
    if hashlib.sha256(archive.read_bytes()).hexdigest() != FFTW_SHA256:
        raise RuntimeError("FFTW source archive checksum mismatch")
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = work / "fftw-3.3.10"
    # The tree may already hold the public header alone (the consumer's
    # include path); the build needs the whole source.
    if not (source / "configure").exists():
        with tarfile.open(archive) as package:
            package.extractall(work)
    build = work / "bitcode"
    build.mkdir(exist_ok=True)

    def run(command):
        print(" ".join(str(item) for item in command), flush=True)
        subprocess.run(command, cwd=build, check=True)

    flags = "-O1 -g -march=" + args.march
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
    # Plain GNU-syntax assembly: no address-significance table (a directive
    # the GNU assembler predates), no debug lines (the translation carries
    # none).
    run([args.clang, "-O3", "-fPIC", "-fno-addrsig", "-S",
         str(work / "fftw-reloc.bc"), "-o", str(work / "fftw-reloc.s")])
    if args.cc:
        run([args.cc, "-c", str(work / "fftw-reloc.s"), "-o",
             str(work / "fftw-reloc.check.o")])
    (work / "fftw-reloc.stamp").write_text(
        "fftw_sha256=" + FFTW_SHA256 + "\n"
        "simd=" + " ".join(args.simd) + "\n"
        "alignment=" + str(alignment) + "\n"
        "march=" + args.march + "\n"
        "clang=" + tool_version(args.clang) + "\n")
    if args.pin:
        pin = Path(args.pin).resolve()
        pin.mkdir(parents=True, exist_ok=True)
        for name in ARTIFACT:
            shutil.copyfile(work / name, pin / name)
        print("pinned " + str(pin), flush=True)


if __name__ == "__main__":
    main()
