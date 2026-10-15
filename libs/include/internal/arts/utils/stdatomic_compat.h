/* SPDX-License-Identifier: Apache-2.0
 *
 * The C11 atomics, spelled once for C and C++.
 *
 * In C this is <stdatomic.h>.  In C++ before C++23 it is the mapping C++23
 * gives <stdatomic.h>: _Atomic(T) names std::atomic<T> and the C names are
 * brought into scope, so one struct definition and one call spelling compile
 * in both languages and lay out identically.  From C++23 on it is the
 * standard header itself.
 *
 * Rules every user follows (the mapping covers exactly what they allow):
 *   - an atomic object is declared with the specifier form _Atomic(T), never
 *     the qualifier form `_Atomic T` and never a <stdatomic.h> typedef name
 *     (atomic_int, atomic_bool, ...), neither of which C++ can map;
 *   - every access is an atomic_*_explicit call with a stated order: never
 *     operator syntax, never a generic function without _explicit, never
 *     memory_order_consume;
 *   - atomic_init only while the object is unpublished, and only in a source
 *     file that is compiled only as C: C++23's <stdatomic.h> does not provide
 *     it, so code C++ also compiles (a header, or a source also built as CUDA
 *     C++) initializes an unpublished object with a relaxed store;
 *   - no arithmetic on an atomic pointer: GCC's C atomic_fetch_add on
 *     _Atomic(T *) adds bytes where Clang and C++ add elements, so a counter
 *     that moves through memory is an _Atomic(uintptr_t).
 *
 * Zero-filled storage (calloc, static) is a valid initial value for every
 * atomic object the runtime declares: each is a lock-free integer, bool or
 * pointer, or a 16-byte {pointer, integer} pair whose all-zero
 * representation is NULL/0.
 */

#ifndef ARTS_UTILS_STDATOMIC_COMPAT_H
#define ARTS_UTILS_STDATOMIC_COMPAT_H

#if !defined(__cplusplus)
#include <stdatomic.h>
#else
/* Runtime headers include one another inside extern "C" blocks, and a
 * template may not have C language linkage. */
extern "C++" {
#if __cplusplus >= 202302L
#include <stdatomic.h>
#else
#include <atomic>

#define _Atomic(T) std::atomic<T>

using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_consume;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

using std::atomic_compare_exchange_strong_explicit;
using std::atomic_compare_exchange_weak_explicit;
using std::atomic_exchange_explicit;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_and_explicit;
using std::atomic_fetch_or_explicit;
using std::atomic_fetch_sub_explicit;
using std::atomic_fetch_xor_explicit;
using std::atomic_is_lock_free;
using std::atomic_load_explicit;
using std::atomic_signal_fence;
using std::atomic_store_explicit;
using std::atomic_thread_fence;
#endif
}
#endif

#endif /* ARTS_UTILS_STDATOMIC_COMPAT_H */
