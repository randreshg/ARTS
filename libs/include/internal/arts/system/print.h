/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/
#ifndef ARTS_SYSTEM_PRINT_H
#define ARTS_SYSTEM_PRINT_H

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "arts/runtime_state.h"
#include "arts/system/identity.h" /* arts_global_rank_id for the log prefix */
#include "arts/system/shutdown.h" /* ARTS_ERROR ends the rank */

#ifdef __cplusplus
extern "C" {
#endif

/*--- One-line stderr output (used by all logging macros) ----------------------
 *
 * Formats into a stack buffer and writes it with one write() when stderr
 * takes it, so that output from concurrent threads does not interleave (a
 * line of at most PIPE_BUF bytes reaches a pipe whole).  Leaves errno as it
 * found it.  It may wait, without bound, until stderr takes the line; a path
 * that must end within a bound writes once with write() instead, or acts
 * before it prints.
 *---------------------------------------------------------------------------*/

/* Writes every byte.  The description behind `fd` may be shared with another
 * process that made it non-blocking (an ssh client does so with the stdout
 * and stderr it inherits); a write it refuses for now is retried once the
 * descriptor can take it, so no line is dropped. */
static inline void arts_write_all(int fd, const char *bytes, size_t length) {
  while (length > 0) {
    const ssize_t written = write(fd, bytes, length);
    if (written > 0) {
      bytes += written;
      length -= (size_t)written;
    } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd writable = {fd, POLLOUT, 0};
      (void)poll(&writable, 1, -1);
    } else if (!(written < 0 && errno == EINTR)) {
      return;
    }
  }
}

static inline void arts_print_line(const char *format, ...) {
  const int given_errno = errno;
  char buffer[4096];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (len > 0) {
    arts_write_all(STDERR_FILENO, buffer,
                   len < (int)sizeof(buffer) ? (size_t)len
                                             : sizeof(buffer) - 1);
  }
  errno = given_errno;
}

/*--- Log Level Macros --------------------------------------------------------
 *
 * ARTS_LOG_LEVEL (set by the build: Debug 3, Release 1 unless chosen; 1 for
 * a file compiled outside it):
 *   0 = ERROR   — print + end the rank (arts_exit with origin RUNTIME_ERROR,
 *                 status 1; it ends the rank even when print is compiled out)
 *   1 = WARN    — print only
 *   2 = INFO    — print only
 *   3 = DEBUG   — print only
 *
 * All levels <= ARTS_LOG_LEVEL are compiled in; a higher level expands to
 * ((void)0), so a compiled-out call is still a statement (an if's body).
 *---------------------------------------------------------------------------*/

#ifndef ARTS_LOG_LEVEL
#define ARTS_LOG_LEVEL 1
#endif

#define ARTS_CLR_RED "\033[1;31m"
#define ARTS_CLR_YELLOW "\033[1;33m"
#define ARTS_CLR_CYAN "\033[36m"
#define ARTS_CLR_DIM "\033[2m"
#define ARTS_CLR_RESET "\033[0m"

/* Level 0: ERROR — the rank's end always follows */
#if ARTS_LOG_LEVEL >= 0
#define ARTS_ERROR(format, ...)                                                \
  do {                                                                         \
    arts_print_line(                                                           \
        ARTS_CLR_RED "[%u:%u] [ERROR] " format ARTS_CLR_RESET "\n",            \
        arts_global_rank_id, arts_thread_info.group_pos, ##__VA_ARGS__);       \
    arts_exit(1, ARTS_EXIT_RUNTIME_ERROR);                                     \
  } while (0)
#else
#define ARTS_ERROR(format, ...)                                                \
  do {                                                                         \
    arts_exit(1, ARTS_EXIT_RUNTIME_ERROR);                                     \
  } while (0)
#endif

/* Level 1: WARN */
#if ARTS_LOG_LEVEL >= 1
#define ARTS_WARN(format, ...)                                                 \
  arts_print_line(                                                             \
      ARTS_CLR_YELLOW "[%u:%u] [WARN] " format ARTS_CLR_RESET "\n",            \
      arts_global_rank_id, arts_thread_info.group_pos, ##__VA_ARGS__)
#else
#define ARTS_WARN(...) ((void)0)
#endif

/* Level 2: INFO */
#if ARTS_LOG_LEVEL >= 2
#define ARTS_INFO(format, ...)                                                 \
  arts_print_line(ARTS_CLR_CYAN "[%u:%u] [INFO] " format ARTS_CLR_RESET "\n",  \
                  arts_global_rank_id, arts_thread_info.group_pos,             \
                  ##__VA_ARGS__)
#else
#define ARTS_INFO(...) ((void)0)
#endif

/* Level 3: DEBUG */
#if ARTS_LOG_LEVEL >= 3
#define ARTS_DEBUG(format, ...)                                                \
  arts_print_line(ARTS_CLR_DIM "[%u:%u] [DEBUG] " format ARTS_CLR_RESET "\n",  \
                  arts_global_rank_id, arts_thread_info.group_pos,             \
                  ##__VA_ARGS__)
#else
#define ARTS_DEBUG(...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARTS_SYSTEM_PRINT_H */
