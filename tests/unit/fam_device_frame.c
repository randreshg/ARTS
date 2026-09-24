/// @file fam_device_frame.c
/// @brief The bootstrap address frame carries {base,size} and demuxes to
/// rank 0.
///
/// Fails on: a frame layout in which the header transfer (offsetof(addr)
/// bytes) does not carry the two fields; and, where device.c is compiled, on a
/// recorder that accepts an arena from a rank other than 0.  Under any other
/// backend only the layout half runs, which is what keeps the frame honest in
/// every tree.
///
/// The struct comes from arts/transport/net.h, which compiles standalone.  A
/// private copy of it would be green whatever the real struct does, because
/// every assertion a copy can make is an assertion about the copy.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "arts/transport/net.h"

int main(void) {
  int fails = 0;
  /* These three are also _Static_assert'd beside the struct and in
   * protocol_abi_asserts.c.  Repeated here as a RUNTIME check so this test
   * fails with a readable message rather than refusing to compile: the header
   * asserts are the guard, this is the diagnosis. */
  if (offsetof(struct arts_net_addr_frame_s, addr) != 24u) {
    printf("FAIL fam_device_frame: addr sits at %zu, expected 24\n",
           offsetof(struct arts_net_addr_frame_s, addr));
    fails++;
  }
  if (offsetof(struct arts_net_addr_frame_s, fam_base) != 8u ||
      offsetof(struct arts_net_addr_frame_s, fam_size) != 16u) {
    printf("FAIL fam_device_frame: the two fields are not where the header "
           "transfer would carry them\n");
    fails++;
  }

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    printf("FAIL fam_device_frame: socketpair\n");
    return 1;
  }
  struct arts_net_addr_frame_s out;
  memset(&out, 0, sizeof(out));
  out.rank = 0;
  out.len = 16;
  out.fam_base = 0x0000123456789000ULL;
  out.fam_size = 64ULL * 1024 * 1024;
  size_t hdr = offsetof(struct arts_net_addr_frame_s, addr);
  if (write(sv[1], &out, hdr) != (ssize_t)hdr) {
    printf("FAIL fam_device_frame: short write\n");
    fails++;
  }
  struct arts_net_addr_frame_s in;
  memset(&in, 0xFF, sizeof(in));
  if (read(sv[0], &in, hdr) != (ssize_t)hdr) {
    printf("FAIL fam_device_frame: short read\n");
    fails++;
  }
  if (in.fam_base != out.fam_base || in.fam_size != out.fam_size ||
      in.rank != out.rank || in.len != out.len) {
    printf("FAIL fam_device_frame: the header transfer lost a field\n");
    fails++;
  }
  close(sv[0]);
  close(sv[1]);

#ifdef ARTS_FAM_BACKEND_DEVICE
  /* Only rank 0's arena is recorded, and a zero base is never recorded. */
  extern void arts_fam_device_record(unsigned from_rank, uint64_t base,
                                     uint64_t size);
  extern uint64_t arts_fam_device_recorded_base(void);
  arts_fam_device_record(1u, 0x1000ULL, 4096ULL);
  if (arts_fam_device_recorded_base() != 0) {
    printf("FAIL fam_device_frame: an arena from rank 1 was recorded\n");
    fails++;
  }
  arts_fam_device_record(0u, 0x2000ULL, 4096ULL);
  if (arts_fam_device_recorded_base() != 0x2000ULL) {
    printf("FAIL fam_device_frame: rank 0's arena was not recorded\n");
    fails++;
  }
#endif

  if (fails) {
    return 1;
  }
  printf("PASS fam_device_frame\n");
  return 0;
}
