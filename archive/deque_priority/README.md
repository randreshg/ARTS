# Archived: priority work-stealing deque (`deque_type=1`)

Archived 2026-05-30 during the cleanup refactor's deque consolidation (the four
deque files — `deque.c` vtable + `deque_simple.c` + `deque_priority.c` + header —
collapsed to a single `deque.c` holding the Chase-Lev work-stealing impl).

`deque_priority.c` is the **priority BST-of-deques** variant of the work-stealing
deque. The runtime selected between the simple Chase-Lev deque (`deque_type=0`)
and this priority variant (`deque_type=1`) via a vtable + `arts_deque_select()`.
No shipped or test config sets `deque_type=1` (the only mention was a commented-out
line in the non-loaded `example.cfg`), so it was **dead in practice** — the
collapse dropped the vtable indirection and kept only the live simple impl.

Preserved because it is a self-contained, modular alternative scheduler-deque
implementation (priority-ordered work stealing) that may be useful if
priority-aware scheduling is revisited.

## Reviving

- The matching vtable ops and the `arts_deque_priority_*` prototypes lived in
  `libs/include/internal/arts/utils/deque.h` (removed in the collapse — recover
  from git `4dc9e92`).
- `struct arts_deque_s` / `circular_array_s` had a **priority-specific layout**
  private to this file (distinct from the simple impl's); a revival must
  reintroduce the vtable (or a compile-time switch) so the two layouts don't
  collide in one `struct arts_deque_s` definition.
- `arts_deque_select(type)` and the `deque_type` config key no longer exist
  (the key is refused at load); a revival must reintroduce both to choose
  this impl.
