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
#include <utility>
#include <vector>

namespace arts_hpx {

// Each caller chooses the application's start and result-completion edges.
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
// states it.  This is the gate for output one locality produces alone; the
// collective form below is the gate for output every locality takes part in,
// and the two are not interchangeable — asking for agreement needs every
// locality to reach the question, which a program whose main runs on one
// locality cannot promise.
inline bool struct_marker()
{
    static bool const on = std::getenv("ARTS_STRUCT_MARKER") != nullptr;
    return on;
}

// The structural pass as every locality agrees on it: decided by locality 0
// and broadcast, so a variable that did not reach every rank fails as a
// mismatch instead of desynchronizing the collectives that follow.  This is
// itself collective, so every locality must reach it, and it must be reached
// once during setup before any task exists; every later call only reads the
// cached answer.
inline bool struct_enabled()
{
    static bool const on = [] {
        auto comm = hpx::collectives::create_communicator("/arts/common/struct",
            hpx::collectives::num_sites_arg(hpx::get_initial_num_localities()),
            hpx::collectives::this_site_arg(hpx::get_locality_id()));
        int flag = struct_marker() ? 1 : 0;
        if (hpx::get_locality_id() == 0)
            return hpx::collectives::broadcast_to(comm, flag).get() == 1;
        return hpx::collectives::broadcast_from<int>(comm).get() == 1;
    }();
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

inline void print_e2e(run_clock const& clock, run_clock const& app_clock)
{
    if (!e2e_enabled() || hpx::get_locality_id() != 0)
        return;
    auto const now = std::chrono::steady_clock::now();
    auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - clock.start).count();
    auto const app_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - app_clock.start).count();
    write_line("[APP_E2E] " + std::to_string(app_ns) + "\n");
    write_line("[E2E] " + std::to_string(ns) + "\n");
}

inline void print_e2e(run_clock const& clock)
{
    print_e2e(clock, clock);
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

// Sums a program's own counters over localities and prints them from
// locality 0.  Every locality calls it (it is a collective), after the end
// stamp, only when struct_enabled().  It registers its communicator's
// basename as it goes, and a basename is registered once, so one program
// calls it at most once.
inline void print_struct(std::vector<std::pair<char const*, std::uint64_t>> const& fields)
{
    std::vector<std::uint64_t> mine;
    for (auto const& f : fields)
        mine.push_back(f.second);
    auto comm = hpx::collectives::create_communicator("/arts/common/struct_sum",
        hpx::collectives::num_sites_arg(hpx::get_initial_num_localities()),
        hpx::collectives::this_site_arg(hpx::get_locality_id()));
    std::vector<std::uint64_t> total =
        hpx::collectives::all_reduce(comm, mine, add_elementwise{}).get();
    if (hpx::get_locality_id() != 0)
        return;
    std::string line = "[STRUCT]";
    for (std::size_t i = 0; i != fields.size(); ++i)
        line += std::string(" ") + fields[i].first + "=" + std::to_string(total[i]);
    write_line(line + "\n");
}

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
