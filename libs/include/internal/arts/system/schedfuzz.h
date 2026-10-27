/* SPDX-License-Identifier: Apache-2.0
 *
 * Probabilistic interleaving-window widener for race reproduction.
 *
 * A rare race needs two threads inside a nanosecond-scale window at once;
 * stress loops hit such windows at rates that make a failure observable but
 * not diagnosable.  Injecting a short randomized delay at the handful of
 * points where lock-free decisions commit widens those windows by orders of
 * magnitude, trading a little wall-clock for reproduction rates
 * that turn a 500-iteration hunt into a 10-iteration one (the randomized-
 * scheduler family of testing techniques).
 *
 * Compiled in only by ARTS_DEBUG_INJECTION, and off unless
 * ARTS_SCHED_FUZZ=<pct> is set in the environment (percentage probability per
 * fuzz point, 0-100, 0 = off).
 */
#ifndef ARTS_SYSTEM_SCHEDFUZZ_H
#define ARTS_SYSTEM_SCHEDFUZZ_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ARTS_DEBUG_INJECTION
/* Read ARTS_SCHED_FUZZ, refusing a value that is not a percentage.  Called at
 * a rank's initialization, before any thread that reaches a fuzz point. */
void arts_sched_fuzz_init(void);
/* Delay this thread 0.5-64us with the configured probability.  Safe from
 * any thread context that may already block (worker or progress dispatch);
 * never call it under a CQ token or inside a signal handler. */
void arts_sched_fuzz_point(void);
#else
#define arts_sched_fuzz_point() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARTS_SYSTEM_SCHEDFUZZ_H */
