/* The serial recursion below the threshold: the origin's
 * fibonacci_serial_sub, statement for statement.  At the row's arguments a
 * run is almost entirely this function, so it is one translation unit that
 * every runtime's program links -- the origin's too -- and the entries then
 * differ in their runtimes and in nothing a compiler did to the kernel.
 *
 * Identical instructions are not yet identical behaviour: where a linker
 * happens to put the function relative to a 64-byte boundary decides how its
 * branches and calls fall in the front end's windows, and moves a function
 * this small and this hot by a tenth either way.  It starts on a boundary in
 * every program, so that too is the same on every side. */
#include <stdint.h>

uint64_t fibonacci_serial_sub(uint64_t n);

__attribute__((noinline, aligned(64))) uint64_t fibonacci_serial_sub(uint64_t n) {
  if (n < 2)
    return n;
  return fibonacci_serial_sub(n - 1) + fibonacci_serial_sub(n - 2);
}
