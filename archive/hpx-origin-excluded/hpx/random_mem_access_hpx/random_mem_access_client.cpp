//  Copyright (c) 2007-2015 Hartmut Kaiser
//  Copyright (c) 2011 Matt Anderson
//  Copyright (c) 2011 Bryce Lelbach
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#if !defined(HPX_COMPUTE_DEVICE_CODE)
#include <hpx/hpx.hpp>
#include <hpx/hpx_init.hpp>

#include <cstddef>
#include <ctime>
#include <random>
#include <vector>

#include <hpx/modules/program_options.hpp>

#include "random_mem_access/random_mem_access.hpp"
#include "common/e2e.hpp"
#include "common/runtime_defaults.hpp"
#include <cstdint>

///////////////////////////////////////////////////////////////////////////////
int hpx_main(hpx::program_options::variables_map& vm)
{
    std::size_t array_size = 0;
    std::size_t iterations = 0;

    if (vm.count("array-size"))
        array_size = vm["array-size"].as<std::size_t>();

    if (vm.count("iterations"))
        iterations = vm["iterations"].as<std::size_t>();

    std::uint64_t seed = std::random_device{}();
    if (vm.count("seed"))
        seed = vm["seed"].as<std::uint64_t>();

    {
        arts_hpx::run_clock clock;
        std::vector<hpx::components::random_mem_access> accu =
            hpx::new_<hpx::components::random_mem_access[]>(
                hpx::default_layout(hpx::find_all_localities()), array_size)
                .get();

        // initialize the array
        std::vector<hpx::future<void>> inits;
        for (std::size_t i = 0; i < array_size; i++)
        {
            inits.push_back(hpx::async<
                hpx::components::server::random_mem_access::init_action>(
                accu[i].get_id(), i));
        }
        hpx::wait_all(inits);

        std::mt19937 gen(static_cast<std::mt19937::result_type>(seed));

        std::vector<hpx::future<void>> barrier;
        for (std::size_t i = 0; i < iterations; i++)
        {
            std::uniform_int_distribution<> dis(
                0, static_cast<int>(array_size - 1));
            std::size_t rn = dis(gen);
            barrier.push_back(accu[rn].add_async());
        }

        hpx::wait_all(barrier);

        std::vector<hpx::future<std::size_t>> counts;
        for (std::size_t i = 0; i < array_size; i++)
        {
            counts.push_back(accu[i].query_async());
        }
        hpx::wait_all(counts);
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < array_size; i++)
            sum += counts[i].get() - i;
        arts_hpx::print_e2e(clock);
        if (arts_hpx::struct_marker())
            arts_hpx::print_parcels();
        arts_hpx::write_stdout_line("COUNT_SUM " + std::to_string(sum) + "\n");
    }

    return hpx::finalize();
}

///////////////////////////////////////////////////////////////////////////////
int main(int argc, char* argv[])
{
    using hpx::program_options::value;

    // Configure application-specific options
    hpx::program_options::options_description desc_commandline(
        "Usage: " HPX_APPLICATION_STRING " [options]");

    desc_commandline.add_options()("array-size",
        value<std::size_t>()->default_value(8), "the size of the array")(
        "iterations", value<std::size_t>()->default_value(16),
        "the number of lookups to perform")(
        "seed", value<std::uint64_t>(), "the seed of the index sequence");
    // Initialize and run HPX
    hpx::init_params init_args;
    init_args.desc_cmdline = desc_commandline;

    arts_hpx::register_geometry_startup();
    init_args.cfg = arts_hpx::runtime_defaults(false);

    return hpx::init(argc, argv, init_args);
}

#endif
