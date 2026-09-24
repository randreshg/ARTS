/// @file fam_shm_handoff.c
/// @brief The pool object is nameless, does not leak, and does reach a rank.
///
/// Standalone: performs the backend's own sequence with no runtime, no
/// launcher, no config file and no ports, so it does not contend with the
/// one-multinode-runner-at-a-time rule.  Fails on: an object with a
/// filesystem name; a new /dev/shm entry; a descriptor an unrelated exec can
/// see; a rank child that cannot see it; two concurrent creators behind one
/// object.

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define POOL_BYTES ((size_t)8 * 1024 * 1024)

static int fails;
#define CHECK(cond, msg)                                                       \
  do {                                                                        \
    if (!(cond)) {                                                            \
      printf("FAIL fam_shm_handoff: %s\n", (msg));                            \
      fails++;                                                                \
    }                                                                         \
  } while (0)

static int dev_shm_entries(void) {
  DIR *d = opendir("/dev/shm");
  if (!d) {
    return -1;
  }
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      n++;
    }
  }
  (void)closedir(d);
  return n;
}

static int make_pool(unsigned char cookie) {
  int fd = memfd_create("arts-fam", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)POOL_BYTES) != 0) {
    return -1;
  }
  void *p = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    close(fd);
    return -1;
  }
  *(unsigned char *)p = cookie;
  return fd;
}

/* Re-execs this program as a reader of fd number `fd`; `clear` says whether
 * the child clears FD_CLOEXEC first, which is what a rank child does. */
static int reader_sees_cookie(const char *self, int fd, bool clear) {
  int fds[2];
  if (pipe(fds) != 0) {
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);
    if (clear) {
      (void)fcntl(fd, F_SETFD, 0);
    }
    char arg[32];
    (void)snprintf(arg, sizeof(arg), "%d", fd);
    execl(self, self, "--read", arg, (char *)NULL);
    _exit(127);
  }
  close(fds[1]);
  char buf[128] = {0};
  (void)read(fds[0], buf, sizeof(buf) - 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  int v = -1;
  (void)sscanf(buf, "%d", &v);
  return v;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--read") == 0) {
    int fd = atoi(argv[2]);
    char link[256] = {0}, path[64];
    (void)snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (readlink(path, link, sizeof(link) - 1) <= 0) {
      printf("-1\n");
      return 0;
    }
    void *p = mmap(NULL, POOL_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    printf("%d\n", p == MAP_FAILED ? -2 : (int)*(unsigned char *)p);
    return 0;
  }

  int before = dev_shm_entries();
  int fd = make_pool(0x5A);
  CHECK(fd >= 0, "the pool object could not be made");
  if (fd < 0) {
    return 1;
  }
  int after = dev_shm_entries();
  CHECK(before == after, "/dev/shm gained an entry: the object has a name");

  char link[256] = {0}, path[64];
  (void)snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  CHECK(readlink(path, link, sizeof(link) - 1) > 0 &&
            strncmp(link, "/memfd:", 7) == 0,
        "the pool's descriptor has a filesystem path");

  CHECK(reader_sees_cookie(argv[0], fd, false) == -1,
        "an unrelated exec inherited the pool's descriptor");
  CHECK(reader_sees_cookie(argv[0], fd, true) == 0x5A,
        "a rank child did not receive the pool");

  /* Two creators at once must end up behind two different objects. */
  int fd_b = make_pool(0xC3);
  CHECK(fd_b >= 0, "a second pool object could not be made");
  CHECK(reader_sees_cookie(argv[0], fd, true) == 0x5A &&
            reader_sees_cookie(argv[0], fd_b, true) == 0xC3,
        "two concurrent runs met behind one object");

  if (fails) {
    return 1;
  }
  printf("PASS fam_shm_handoff\n");
  return 0;
}
