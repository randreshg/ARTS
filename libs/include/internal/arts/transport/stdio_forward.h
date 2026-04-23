/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
#ifndef ARTS_TRANSPORT_STDIO_FORWARD_H
#define ARTS_TRANSPORT_STDIO_FORWARD_H

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * arts_stdio_forwarder_alloc_pipe — Create a pipe pair and reserve a
 * forwarder slot, but do NOT spawn the reader thread yet.
 *
 * This is the first half of the two-phase API that must be used whenever
 * the caller will call fork() after obtaining the write-end fd.  Spawning
 * pthreads before fork() is unsafe when the process holds library state
 * with internal locks (e.g. the CXL Rapid API allocator): the child
 * inherits a locked mutex owned by a thread that no longer exists, causing
 * a deadlock before the child ever reaches bind()/listen().
 *
 * Call arts_stdio_forwarder_start_threads() once, in the parent, after
 * ALL forks for the current launch batch are complete.
 *
 * @param rank         Child rank (used for diagnostics).
 * @param stream_label "stdout" or "stderr" (borrowed string literal).
 * @param sink         Master's FILE* to write forwarded lines to.
 * @return Write-end fd (>=0) on success — caller dup2's this into the
 *         child before exec, then closes their copy in the parent.
 *         Returns -1 on failure; caller should fall back to /dev/null.
 */
int arts_stdio_forwarder_alloc_pipe(unsigned int rank,
                                    const char *stream_label, FILE *sink);

/**
 * arts_stdio_forwarder_start_threads — Spawn reader threads for every
 * slot that was allocated by arts_stdio_forwarder_alloc_pipe() but has
 * not yet had its thread started.
 *
 * Call this ONCE in the parent process after the entire fork loop
 * completes (all children have been forked and the parent has closed its
 * copies of the write-ends).
 */
void arts_stdio_forwarder_start_threads(void);

/**
 * arts_stdio_forwarder_make_pipe — Convenience wrapper: alloc_pipe +
 * start_threads in one call.  Safe to use only when no fork() will follow
 * before the thread is joined (i.e. non-forking callers).
 *
 * @param rank         Child rank used in the prefix.
 * @param stream_label "stdout" or "stderr" (borrowed string literal).
 * @param sink         Master's FILE* to write forwarded lines to.
 * @return Write-end fd (>=0) on success, -1 on failure.
 */
int arts_stdio_forwarder_make_pipe(unsigned int rank, const char *stream_label,
                                   FILE *sink);

/**
 * arts_stdio_forwarder_shutdown_all — Join all reader threads and close
 * their fds.  Call AFTER the launcher has waitpid'd all child processes
 * (so pipe EOF has propagated and threads have exited their read loop).
 * Idempotent.
 */
void arts_stdio_forwarder_shutdown_all(void);

#ifdef __cplusplus
}
#endif
#endif /* ARTS_TRANSPORT_STDIO_FORWARD_H */
