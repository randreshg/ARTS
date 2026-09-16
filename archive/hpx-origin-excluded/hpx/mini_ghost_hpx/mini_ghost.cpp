//  Copyright (c) 2014 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_init.hpp>
#include <hpx/hpx.hpp>
#include <hpx/semaphore.hpp>

#include <profiling.hpp>
#include <barrier.hpp>
#include <params.hpp>
#include <stepper.hpp>

#include "common/e2e.hpp"
#include "common/runtime_defaults.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>

typedef mini_ghost::grid<double> grid_type;

typedef mini_ghost::stepper<grid_type::value_type> stepper_type;

// Global configuration data (after initialization this is read-only)
mini_ghost::params<grid_type::value_type> p;

// Global profiling data
hpx::spinlock profiling_data_mtx;
std::shared_ptr<hpx::counting_semaphore_var<>> profiling_data_sem;
std::vector<mini_ghost::profiling::profiling_data> profiling_data;

double init_start = 0;

// The largest relative error the conservation check has seen on this
// locality; the check itself is the origin's, once per step per summed
// variable.
std::atomic<double> g_err_max(0.0);

void add_profile(mini_ghost::profiling::profiling_data const & pd)
{
    {
        std::lock_guard<hpx::spinlock> l(profiling_data_mtx);
        profiling_data.push_back(pd);
        profiling_data_sem->signal();
    }

    if (p.rank == 0)
    {
        profiling_data_sem->wait();
    }
}

HPX_PLAIN_ACTION(add_profile);

int hpx_main(hpx::program_options::variables_map& vm)
{
    mini_ghost::profiling::data().time_init(
        hpx::chrono::high_resolution_timer::now() - init_start);

    hpx::chrono::high_resolution_timer timer_all;

    hpx::id_type here = hpx::find_here();
    std::string name = hpx::get_locality_name();

    p.rank = hpx::naming::get_locality_id_from_id(here);
    if(p.rank == 0)
    {
        std::cout << "mini ghost started up in "
                  << hpx::chrono::high_resolution_timer::now() - init_start
                  << " seconds.\n";
    }

    p.nranks = hpx::get_num_localities(hpx::launch::sync);

    profiling_data_sem.reset(new hpx::counting_semaphore_var<>(p.nranks));
    p.setup(vm);

    arts_hpx::print_geometry();

    // Create the local stepper object, retrieve the local pointer to it
    hpx::id_type stepper_id = hpx::components::new_<stepper_type>(here).get();
    std::shared_ptr<stepper_type> stepper(
        hpx::get_ptr<stepper_type>(stepper_id).get());

    // Initialize stepper
    arts_hpx::run_clock clock;
    stepper->init(p).get();
    mini_ghost::barrier_wait();

    // Perform the actual simulation work
    stepper->run(p.num_spikes, p.num_tsteps);
    mini_ghost::barrier_wait();
    arts_hpx::print_e2e(clock);
    if (arts_hpx::struct_marker())
        arts_hpx::print_parcels();

    // Output various pieces of information about the run
    if (stepper->get_rank() == 0)
    {
        char line[64];
        std::snprintf(line, sizeof line, "ERRMAX %.6e\n", g_err_max.load());
        arts_hpx::write_stdout_line(line);

        // Output various pieces of information about the run
        add_profile(mini_ghost::profiling::data());

        if (p.report_perf)
            mini_ghost::profiling::report(std::cout, profiling_data, p);
        else
            std::cout << "Total runtime: " << timer_all.elapsed() << "\n";

        std::ofstream fs("results.yaml");
        mini_ghost::profiling::report(fs, profiling_data, p);
        std::cout << "finalizing ...\n";

        return hpx::finalize();
    }
    else
    {
        // Send performance data from this locality to root
        hpx::post(add_profile_action(), hpx::find_root_locality(),
            mini_ghost::profiling::data());
        return 0;
    }
}

int main(int argc, char* argv[])
{
    // Configure application-specific options
    hpx::program_options::options_description
       desc_commandline("Usage: " HPX_APPLICATION_STRING " [options]");

    p.cmd_options(desc_commandline);

    // Register startup functions for creating/retrieving our global barrier
    hpx::register_pre_startup_function(&mini_ghost::create_barrier);
    hpx::register_startup_function(&mini_ghost::find_barrier);

    // Initialize and run HPX such that hpx_main is executed on all localities
    hpx::init_params init_args;
    init_args.desc_cmdline = desc_commandline;
    init_args.cfg = arts_hpx::runtime_defaults();

    init_start = hpx::chrono::high_resolution_timer::now();
    return hpx::init(argc, argv, init_args);
}
