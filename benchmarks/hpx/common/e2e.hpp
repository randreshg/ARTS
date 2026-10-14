#pragma once

#include <hpx/hpx.hpp>
#include <hpx/include/performance_counters.hpp>
#include <hpx/modules/collectives.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace arts_hpx {

// The span every runtime of the comparison reports, stamped on locality 0
// alone: opened by the first statement of its hpx_main, closed where that
// same locality asks the runtime to stop.  Runtime start-up and teardown lie
// outside it; everything the program does lies inside it.
struct run_clock
{
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
};

inline bool e2e_enabled()
{
    static bool const on = std::getenv("ARTS_E2E_MARKER") != nullptr;
    return on;
}

// Whether the structural pass is on, as this locality's own environment
// states it: the gate for output one locality produces alone, which asks
// nothing of the other localities.
inline bool struct_marker()
{
    static bool const on = std::getenv("ARTS_STRUCT_MARKER") != nullptr;
    return on;
}

// One write per line: the launcher merges every locality's stream into one
// and the driver parses whole lines, so a line split across two writes can
// be interleaved with another stream's line.  A short or interrupted write
// is resumed rather than dropped — a truncated line is a lost record, not a
// cosmetic defect.
inline void write_fd_line(int fd, std::string const& line)
{
    char const* cursor = line.data();
    std::size_t left = line.size();
    while (left != 0)
    {
        ssize_t const n = ::write(fd, cursor, left);
        if (n > 0)
        {
            cursor += n;
            left -= static_cast<std::size_t>(n);
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return;    // the stream is gone; there is nowhere left to report
        }
    }
}

inline void write_line(std::string const& line)
{
    write_fd_line(2, line);
}

inline void write_stdout_line(std::string const& line)
{
    write_fd_line(1, line);
}

inline void print_geometry()
{
    if (!e2e_enabled())
        return;
    write_line("[HPX] locality=" + std::to_string(hpx::get_locality_id()) +
        " localities=" + std::to_string(hpx::get_initial_num_localities()) +
        " threads=" + std::to_string(hpx::get_os_thread_count()) + "\n");
}

// A program whose hpx_main runs on locality 0 alone still owes one geometry
// line per locality; a startup function runs on every locality once the
// runtime is up and before hpx_main.
inline void register_geometry_startup()
{
    hpx::register_startup_function([] { print_geometry(); });
}

inline void print_e2e(run_clock const& clock)
{
    if (!e2e_enabled() || hpx::get_locality_id() != 0)
        return;
    auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - clock.start).count();
    write_line("[E2E] " + std::to_string(ns) + "\n");
}

// A reduction operator travels to the other sites as an action argument, so
// it must be default-constructible: the receiving side default-constructs
// the argument before loading it.  A closure type is not, whatever it
// captures, so the operator is a named empty struct.
struct add_elementwise
{
    std::vector<std::uint64_t> operator()(
        std::vector<std::uint64_t> a, std::vector<std::uint64_t> const& b) const
    {
        std::size_t const n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i != n; ++i)
            a[i] += b[i];
        return a;
    }
};

// Sums one counter over every locality.  Each locality's instance is named
// outright rather than through the `locality#*` wildcard: the wildcard needs
// a discovery pass that this version resolves to an empty set in-process,
// and reading a named instance is a plain request to the locality that owns
// it, so the caller needs no participation from the others — which is what
// lets this be called from a program whose main runs on one locality alone.
inline std::uint64_t counter_total(
    std::string const& object, std::string const& path)
{
    std::uint64_t sum = 0;
    std::uint32_t const localities = hpx::get_initial_num_localities();
    for (std::uint32_t i = 0; i != localities; ++i)
    {
        hpx::performance_counters::performance_counter counter(
            object + "{locality#" + std::to_string(i) + "/total}" + path);
        sum += static_cast<std::uint64_t>(
            counter.get_value<std::int64_t>(hpx::launch::sync));
    }
    return sum;
}

// The runtime's own parcel counters, summed over localities: an upper bound
// on the wire traffic (no coalescing is configured, and the remote reads
// this function performs are parcels themselves, a fixed few per locality),
// printed once by locality 0, after the end stamp, when the structural pass
// is on.  `bytes` is the
// argument data a parcel carried, `wire` the serialized parcel including its
// headers — the second is what a message census on another runtime's
// transport counts.  A single locality never instantiates a parcelport, so
// its counters are not registered and the traffic they would measure is zero.
inline void print_parcels()
{
    if (hpx::get_locality_id() != 0)
        return;
    std::uint64_t sent = 0, bytes = 0, wire = 0;
    if (hpx::get_initial_num_localities() > 1)
    {
        sent = counter_total("/parcels", "/count/mpi/sent");
        bytes = counter_total("/data", "/count/mpi/sent");
        wire = counter_total("/serialize", "/count/mpi/sent");
    }
    write_line("[PARCELS] sent=" + std::to_string(sent) +
        " bytes=" + std::to_string(bytes) + " wire=" + std::to_string(wire) +
        "\n");
}

}    // namespace arts_hpx
