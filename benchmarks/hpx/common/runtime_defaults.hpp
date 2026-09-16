#pragma once

#include <string>
#include <vector>

namespace arts_hpx {

// An externally installed affinity mask is authoritative and SMT siblings
// never carry a worker; sends go out immediately rather than parking on a
// cached connection slot; hpx_main runs on every locality, since every
// locality owns setup state and must stay inside hpx_main until the global
// completion edge (finalize on any locality begins global shutdown).
// A program written for a single hpx_main keeps it: forcing hpx_main onto
// every locality would run its driver loop once per locality.
//
// The last two lines are one budget between them.  A thread object that runs
// takes a stack and a guard page, and its queue keeps the stack in a cache
// for the rest of the run, so a process's mapping count follows how many
// thread objects it ever materialised rather than how many are live at once.
// The per-queue ceiling caps only what a queue materialises ahead of demand
// and is soft — a queue that runs dry raises its own limit — so it trims a
// burst instead of bounding one.  What bounds the materialised set is the
// pending queue's pop order: LIFO makes a worker run its own newest child
// first, so an eagerly spawned tree is walked depth-first and its frontier
// stays on the order of the tree's depth, while FIFO expands it
// breadth-first and turns the whole frontier into started thread objects.
// Owner and thief take from that same end here: the pending queue is a
// stack, not a work-stealing deque, and no scheduler factory in this
// version instantiates the ABP deque backends the policy names suggest.
// That line alone carries no forcing modifier: a forcing entry outranks the
// command line, and a scheduling policy must stay selectable there.
inline std::vector<std::string> runtime_defaults(bool run_main_everywhere = true)
{
    std::vector<std::string> cfg = {"hpx.use_process_mask!=1",
        "hpx.os_threads!=cores", "hpx.parcel.mpi.sendimm!=1",
        "hpx.thread_queue.max_thread_count!=100",
        "hpx.scheduler=local-priority-lifo"};
    // A program whose hpx_main runs on locality 0 alone keeps that shape:
    // the other localities host its components and leave when it finalizes.
    if (run_main_everywhere)
        cfg.push_back("hpx.run_hpx_main!=1");
    return cfg;
}

}    // namespace arts_hpx
