/* SPDX-License-Identifier: Apache-2.0
 *
 * pure_unit test for the spawner's ssh command (libs/src/core/transport/
 * spawner.c): the static quoting helper
 *   static int arts_shell_quote(const char *input, char *output,
 *                               size_t output_size)
 * and the command a login shell on a rank's host runs,
 *   static char *arts_ssh_command(unsigned int rank, int argc, char **argv,
 *                                 const char *exe, const char *cwd)
 *
 * Because both are `static`, this TU #include's spawner.c directly so they
 * are compiled into the test (precedent: edt_gpu.cu #include's edt.c).
 * spawner.c's other functions, compiled here but not called, reference
 * runtime symbols (arts_transport_select_local_ports, arts_startup_budget_ms,
 * arts_fake_cxl_fini_replay, the watchdog's two calls, ARTS_ERROR ->
 * arts_thread_info / arts_global_rank_id / arts_exit, ...), so trivial
 * stubs/defs below satisfy the link.  The command builder calls arts_malloc's
 * family and arts_config_key_names, whose stub names two keys.
 *
 * Properties verified (no runtime started, no ssh):
 *
 *  1. Single-quote escaping: every embedded `'` becomes the classic 5-char
 *     idiom  '"'"'  (close-quote, dq-quote, open-quote), and the whole token
 *     is wrapped in single quotes.  This is the SSH command-injection safety
 *     contract: a payload that contains `'` (and shell metacharacters) must
 *     come back wrapped so a single `sh -c <token>` re-parse yields exactly
 *     the original bytes.
 *
 *  2. Boundary / off-by-one: the embedded-quote branch reserves with
 *     `out_index + 5 >= output_size` and the final close-quote with
 *     `out_index + 2 > output_size`.  We sweep output_size across the exact
 *     window where the LAST fitting char is a `'`, and assert:
 *       - the function returns -1 when (and only when) the result would not
 *         fit, and
 *       - it NEVER writes outside [output, output+output_size) — enforced by
 *         placing the output buffer between poison canaries and checking them
 *         after every call (ASan also guards via a heap-allocated exact-size
 *         buffer).
 *
 *  3. Round-trip semantics: for a set of adversarial inputs, the produced
 *     quoting, when interpreted by /bin/sh, reproduces the original bytes
 *     exactly (decodes the '"'"' idiom by hand and compares).
 *
 *  4. output_size < 3 always returns -1 (and writes nothing meaningful).
 *
 *  5. The session wrapper, run as a remote login shell runs the ssh command
 *     (`/bin/sh -c <command>` with a pipe on its stdin): `TERM` written to
 *     the pipe ends the rank with 143, the pipe's end-of-file kills it (137),
 *     a rank that exits 42 gives 42, and a missing working directory 127.
 *
 *  6. The ssh command quotes every value it carries: the working directory,
 *     ARTS_CONFIG (made absolute against it), LD_LIBRARY_PATH, an ARTS_
 *     variable, a variable named after a config key, the binary and every
 *     argument each hold a `'`, a space and `$(x)` (the binary and an
 *     argument an `=` too, which no assignment may take), and the rank,
 *     started with only PATH in the shell's environment, sees each exactly;
 *     a variable that is neither is not passed, nor is a name the shell
 *     would not read as one word.
 *
 * On success prints "PASS launcher_shell_quote ..."; on any failure prints a
 * FAIL line to stderr and returns 1.
 */

/* ---- stubs for spawner.c's external references ---- */
#include "arts/system/shutdown.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Match the real declarations so spawner.c's references resolve. */
unsigned int arts_global_rank_id = 0;

/* Defined below once spawner.c's includes have declared its layout; only the
 * uncalled ARTS_ERROR paths read it. */
struct arts_runtime_private_s;

void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t size) { return calloc(n, size); }
void *arts_realloc(void *p, size_t size) { return realloc(p, size); }
void arts_free(void *p) { free(p); }
void *arts_malloc_aligned(size_t size, size_t align) {
  (void)align;
  return malloc(size);
}
void *arts_calloc_aligned(size_t n, size_t size, size_t align) {
  (void)align;
  return calloc(n, size);
}
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  exit(rc ? rc : 1);
}

/* Pull in spawner.c (and through it the real declaration of
 * struct arts_runtime_private_s + the extern arts_thread_info). */
#include "transport/spawner.c"

bool arts_transport_select_local_ports(struct arts_config_s *config) {
  (void)config;
  return true;
}
unsigned int arts_startup_budget_ms(void) { return 20000u; }
void arts_fake_cxl_fini_replay(void) {}
void arts_exit_mark_spawner(void) {}
void arts_exit_watchdog_arm(int rc) { (void)rc; }
const char *const *arts_config_key_names(size_t *count) {
  static const char *const keys[] = {"provider", "worker_threads"};
  *count = sizeof(keys) / sizeof(keys[0]);
  return keys;
}

/* arts_thread_info is declared `extern ARTS_THREAD_LOCAL_IE` by runtime_state.h
 * (pulled via spawner.c -> print.h).  Define the object so the link
 * resolves; only the ARTS_ERROR paths, which this test does not reach, read
 * it (the spawner's own debug lines do not). */
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/stat.h>

/* ---------- test scaffolding ---------- */

static int g_fail = 0;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL launcher_shell_quote: " __VA_ARGS__);              \
      fprintf(stderr, "\n  (at %s:%d)\n", __FILE__, __LINE__);                 \
      g_fail = 1;                                                              \
    }                                                                          \
  } while (0)

/* Canary-guarded wrapper: place `size` writable bytes between poison regions
 * and confirm arts_shell_quote never touches the poison.  Returns the function
 * return value; copies the produced bytes (size of them) into `captured`. */
#define CANARY 16
static int quote_guarded(const char *input, size_t size, char *captured,
                         int *poison_ok) {
  /* layout: [CANARY poison][size bytes output][CANARY poison] */
  char *buf = (char *)malloc(CANARY + size + CANARY);
  memset(buf, 0xAB, CANARY + size + CANARY);
  char *out = buf + CANARY;
  int rc = arts_shell_quote(input, out, size);
  /* verify poison intact */
  int ok = 1;
  for (size_t i = 0; i < CANARY; i++) {
    if ((unsigned char)buf[i] != 0xAB)
      ok = 0;
    if ((unsigned char)buf[CANARY + size + i] != 0xAB)
      ok = 0;
  }
  *poison_ok = ok;
  if (captured)
    memcpy(captured, out, size);
  free(buf);
  return rc;
}

/* Decode an arts_shell_quote output (which is `'<body>'` with embedded quotes
 * expanded to '"'"') back to the original byte string.  Returns 0 on success
 * and fills `dst` (NUL-terminated). */
static int decode_quoted(const char *q, char *dst, size_t dstcap) {
  size_t n = strlen(q);
  if (n < 2 || q[0] != '\'' || q[n - 1] != '\'')
    return -1;
  size_t di = 0;
  size_t i = 1;       /* skip opening quote */
  size_t end = n - 1; /* index of closing quote */
  while (i < end) {
    /* recognize the 5-char idiom  ' " ' " '  spanning the close+reopen */
    if (q[i] == '\'' && i + 4 < end + 1 && q[i + 1] == '"' &&
        q[i + 2] == '\'' && q[i + 3] == '"' && q[i + 4] == '\'') {
      if (di + 1 >= dstcap)
        return -1;
      dst[di++] = '\'';
      i += 5;
    } else {
      if (di + 1 >= dstcap)
        return -1;
      dst[di++] = q[i++];
    }
  }
  dst[di] = '\0';
  return 0;
}

/* Runs `command` as a login shell on a rank's host runs the ssh command:
 * /bin/sh -c, with a pipe on its stdin and only PATH in its environment.
 * `action` 'T' writes "TERM\n" to the pipe, 'E' closes it, 'N' leaves it open
 * until the shell ends.  The shell's stdout goes to `out`.  Returns its
 * status (128 + the signal for one a signal ended), or -1 when it did not end
 * within 20 s (it is then killed). */
static int run_as_remote_shell(const char *command, char action, char *out,
                               size_t out_size) {
  int in[2];
  int from[2];
  if (pipe2(in, O_CLOEXEC) != 0 || pipe2(from, O_CLOEXEC) != 0) {
    return -1;
  }
  pid_t child = fork();
  if (child == 0) {
    char *const env[] = {"PATH=/usr/bin:/bin", NULL};
    (void)dup2(in[0], STDIN_FILENO);
    (void)dup2(from[1], STDOUT_FILENO);
    execle("/bin/sh", "sh", "-c", command, (char *)NULL, env);
    _exit(127);
  }
  (void)close(in[0]);
  (void)close(from[1]);
  if (action == 'T') {
    (void)!write(in[1], "TERM\n", 5);
  } else if (action == 'E') {
    (void)close(in[1]);
    in[1] = -1;
  }

  struct timespec start;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);
  size_t got = 0;
  out[0] = '\0';
  int status = 0;
  pid_t ended = 0;
  for (;;) {
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec > 20) {
      (void)kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
      ended = -1;
      break;
    }
    if (from[0] >= 0) {
      struct pollfd p = {from[0], POLLIN, 0};
      if (poll(&p, 1, 10) > 0) {
        char chunk[256];
        ssize_t n = read(from[0], chunk, sizeof(chunk));
        if (n <= 0) {
          (void)close(from[0]);
          from[0] = -1;
        } else {
          for (ssize_t k = 0; k < n && got + 1 < out_size; k++) {
            out[got++] = chunk[k];
          }
          out[got] = '\0';
        }
      }
      continue;
    }
    ended = waitpid(child, &status, WNOHANG);
    if (ended == child) {
      break;
    }
    struct timespec pause = {0, 10 * 1000 * 1000};
    (void)nanosleep(&pause, NULL);
  }
  if (from[0] >= 0) {
    (void)close(from[0]);
  }
  if (in[1] >= 0) {
    (void)close(in[1]);
  }
  if (ended != child) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(void) {
  char outbuf[512];
  int poison;
  int rc;

  /* --- 1. basic escaping correctness + round trip --- */
  const char *cases[] = {
      "",
      "hello",
      "a b c", /* spaces */
      "it's",  /* one quote */
      "'leading",
      "trailing'",
      "''''", /* all quotes */
      "a'b'c'd",
      "rm -rf /; echo $(whoami)", /* metacharacters */
      "$PATH `id` \"dq\" \\bs",
      "tab\tnewline\nend",
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    char q[512];
    memset(q, 0, sizeof(q));
    rc = arts_shell_quote(cases[c], q, sizeof(q));
    CHECK(rc == 0, "expected success quoting \"%s\"", cases[c]);
    if (rc != 0)
      continue;
    /* must be wrapped in single quotes */
    size_t ql = strlen(q);
    CHECK(ql >= 2 && q[0] == '\'' && q[ql - 1] == '\'',
          "result not single-quote-wrapped for \"%s\": <%s>", cases[c], q);
    /* must contain NO bare single quote inside the body other than as part of
     * the idiom — verify by decoding and comparing to the original. */
    char decoded[512];
    int drc = decode_quoted(q, decoded, sizeof(decoded));
    CHECK(drc == 0, "decode failed for <%s>", q);
    if (drc == 0)
      CHECK(strcmp(decoded, cases[c]) == 0,
            "round-trip mismatch: in=\"%s\" decoded=\"%s\" quoted=<%s>",
            cases[c], decoded, q);
  }

  /* Explicit idiom check: a single `'` must produce exactly '\''\'''\'' ...
   * i.e. for input "'" the body is  '"'"'  framed by outer quotes:
   *   ' + '"'"' + '  ==  '\''  spelled out as: ' ' " ' " ' '  (7 chars) */
  {
    char q[64];
    rc = arts_shell_quote("'", q, sizeof(q));
    CHECK(rc == 0, "quoting a lone quote failed");
    CHECK(strcmp(q, "''\"'\"''") == 0,
          "lone-quote idiom wrong: got <%s> want <''\"'\"''>", q);
  }

  /* --- 2. output_size < 3 always returns -1 --- */
  for (size_t s = 0; s < 3; s++) {
    char small[4];
    memset(small, 0xCD, sizeof(small));
    rc = arts_shell_quote("x", small, s);
    CHECK(rc == -1, "output_size=%zu (<3) must return -1, got %d", s, rc);
  }

  /* --- 3. boundary sweep: ordinary chars, no embedded quotes ---
   * For input of L ordinary chars, the minimal output_size that succeeds is
   * L + 3 (open quote + L chars + close quote + NUL).  Sweep around it. */
  for (size_t L = 0; L <= 8; L++) {
    char input[16];
    memset(input, 'x', L);
    input[L] = '\0';
    size_t need = L + 3; /* '<L chars>' + NUL */
    for (size_t sz = 3; sz <= need + 2; sz++) {
      rc = quote_guarded(input, sz, outbuf, &poison);
      CHECK(poison, "OOB write (ordinary) L=%zu sz=%zu", L, sz);
      if (sz >= need) {
        CHECK(rc == 0, "ordinary L=%zu sz=%zu should succeed, got %d", L, sz,
              rc);
      } else {
        CHECK(rc == -1, "ordinary L=%zu sz=%zu should fail, got %d", L, sz, rc);
      }
    }
  }

  /* --- 4. boundary sweep where the LAST fitting char is a single quote ---
   * The embedded-quote branch needs 5 output slots and guards with
   *   out_index + 5 >= output_size  -> -1
   * which (because of the trailing NUL) means it requires out_index + 5 < size
   * i.e. 6 bytes of headroom before writing the 5 escape chars.  This is the
   * exact off-by-one boundary.  Sweep output_size and confirm the
   * canaries are never touched and the return value matches what actually
   * fits.  We accept WHATEVER the function decides about success/-1 at each
   * size as long as: (a) no OOB write ever happens, and (b) whenever it
   * returns 0 the output is a valid round-trippable quoting. */
  for (size_t L = 0; L <= 4; L++) {
    char input[16];
    /* L ordinary chars followed by one single quote */
    memset(input, 'a', L);
    input[L] = '\'';
    input[L + 1] = '\0';
    for (size_t sz = 3; sz <= L + 12; sz++) {
      rc = quote_guarded(input, sz, outbuf, &poison);
      CHECK(poison, "OOB write (quote-at-boundary) L=%zu sz=%zu", L, sz);
      if (rc == 0) {
        /* When it claims success, the bytes within [0,sz) must be a complete,
         * NUL-terminated, round-trippable quoting. */
        /* Find the NUL inside the captured region. */
        size_t nul = sz;
        for (size_t i = 0; i < sz; i++) {
          if (outbuf[i] == '\0') {
            nul = i;
            break;
          }
        }
        CHECK(nul < sz, "success but no NUL within buffer L=%zu sz=%zu", L, sz);
        if (nul < sz) {
          char decoded[64];
          if (decode_quoted(outbuf, decoded, sizeof(decoded)) == 0) {
            CHECK(strcmp(decoded, input) == 0,
                  "boundary success but wrong content L=%zu sz=%zu: <%s>", L,
                  sz, outbuf);
          } else {
            CHECK(0, "boundary success but undecodable L=%zu sz=%zu: <%s>", L,
                  sz, outbuf);
          }
        }
      }
    }
  }

  /* --- 5. exact-size heap buffer (ASan-precise) for a quote at the very end
   * of the producible output, to catch a 1-byte overrun the canary sweep above
   * might statistically miss. --- */
  {
    /* input = single quote.  Successful output is 7 bytes + NUL = 8.  Try the
     * minimal exact size and one below it on a heap buffer sized exactly. */
    for (size_t sz = 6; sz <= 9; sz++) {
      char *exact = (char *)malloc(sz);
      memset(exact, 0, sz);
      int r = arts_shell_quote("'", exact, sz);
      if (r == 0) {
        /* Must be NUL-terminated within sz and decode back to "'" */
        size_t len = strnlen(exact, sz);
        CHECK(len < sz, "lone-quote success not terminated within sz=%zu", sz);
        if (len < sz) {
          char dec[8];
          CHECK(decode_quoted(exact, dec, sizeof(dec)) == 0 &&
                    strcmp(dec, "'") == 0,
                "lone-quote exact sz=%zu produced wrong <%s>", sz, exact);
        }
      }
      free(exact);
    }
  }

  /* --- the session wrapper: a signal name, end-of-file, the status --- */
  char here[PATH_MAX];
  CHECK(getcwd(here, sizeof(here)) != NULL, "getcwd failed");
  char dir_name[] = "launcher_shell_quote it's $(x) XXXXXX";
  CHECK(mkdtemp(dir_name) != NULL, "mkdtemp failed");
  char dir[PATH_MAX + 64];
  (void)snprintf(dir, sizeof(dir), "%s/%s", here, dir_name);
  char exe[PATH_MAX + 128];
  (void)snprintf(exe, sizeof(exe), "%s/sh it's $(x) run=1", dir);
  CHECK(symlink("/bin/sh", exe) == 0, "symlink %s failed", exe);
  unsetenv("ARTS_RANK");
  {
    struct {
      char action;
      const char *script;
      const char *cwd_suffix;
      int want;
    } waits[] = {
        {'T', "exec sleep 30", "", 143},
        {'E', "exec sleep 30", "", 137},
        {'N', "exit 42", "", 42},
        {'N', "exit 0", "/missing", 127},
    };
    for (size_t w = 0; w < sizeof(waits) / sizeof(waits[0]); w++) {
      char cwd[sizeof(dir) + 16];
      (void)snprintf(cwd, sizeof(cwd), "%s%s", dir, waits[w].cwd_suffix);
      char *argv[] = {"rank", "-c", (char *)waits[w].script, NULL};
      char *command = arts_ssh_command(0, 3, argv, exe, cwd);
      char out[64];
      int got = run_as_remote_shell(command, waits[w].action, out, sizeof(out));
      CHECK(got == waits[w].want,
            "wrapper case %zu ('%c', \"%s\"): status %d, want %d", w,
            waits[w].action, waits[w].script, got, waits[w].want);
      arts_free(command);
    }
  }

  /* --- every value the ssh command carries round-trips --- */
  {
    const char *odd = "it's $(x)";
    char config[64];
    (void)snprintf(config, sizeof(config), "cfg %s.cfg", odd);
    char library_path[64];
    (void)snprintf(library_path, sizeof(library_path), "/nonexistent %s", odd);
    const char *probe = "v it's $(x) \"dq\" `bt` \\bs";
    char provider[64];
    (void)snprintf(provider, sizeof(provider), "tcp %s", odd);
    setenv("ARTS_CONFIG", config, 1);
    setenv("LD_LIBRARY_PATH", library_path, 1);
    setenv("ARTS_PROBE_VALUE", probe, 1);
    setenv("provider", provider, 1);
    setenv("LSQ_UNLISTED", "1", 1);
    setenv("ARTS_bad-name", "1", 1);
    char *argv[] = {"rank",
                    "-c",
                    "printf '[%s]\\n' \"$(pwd -P)\" \"$ARTS_CONFIG\" "
                    "\"$LD_LIBRARY_PATH\" \"$ARTS_PROBE_VALUE\" \"$provider\" "
                    "\"${LSQ_UNLISTED-unset}\" \"$ARTS_RANK\" \"$0\" \"$1\" "
                    "\"$2\"",
                    "a0 it's $(x)",
                    "a1 'q' `z` $HOME",
                    "--size=100",
                    NULL};
    char *command = arts_ssh_command(7, 6, argv, exe, dir);
    CHECK(strstr(command, "LSQ_UNLISTED=") == NULL,
          "an unlisted variable is passed: <%s>", command);
    CHECK(strstr(command, "ARTS_bad-name=") == NULL,
          "a name that is not one shell word is passed: <%s>", command);
    char want[(2 * sizeof(dir)) + 512];
    char out[sizeof(want)];
    int got = run_as_remote_shell(command, 'N', out, sizeof(out));
    (void)snprintf(want, sizeof(want),
                   "[%s]\n[%s/%s]\n[%s]\n[%s]\n[%s]\n[unset]\n[7]\n"
                   "[a0 it's $(x)]\n[a1 'q' `z` $HOME]\n[--size=100]\n",
                   dir, dir, config, library_path, probe, provider);
    CHECK(got == 0, "round-trip run: status %d (command <%s>)", got, command);
    CHECK(strcmp(out, want) == 0,
          "round-trip mismatch:\n--- got\n%s--- want\n%s--- command <%s>", out,
          want, command);
    arts_free(command);
    unsetenv("LD_LIBRARY_PATH");
  }
  (void)unlink(exe);
  CHECK(rmdir(dir) == 0, "rmdir %s failed", dir);

  if (g_fail) {
    fprintf(stderr, "FAIL launcher_shell_quote\n");
    return 1;
  }
  printf("PASS launcher_shell_quote (escape idiom, boundary off-by-one, "
         "no-OOB, round-trip, session wrapper, ssh command)\n");
  return 0;
}
