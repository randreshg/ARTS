
//  Copyright (c) 2012 Thomas Heller
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#if !defined(HPX_COMPUTE_DEVICE_CODE)
#include <hpx/chrono.hpp>
#include <hpx/init.hpp>

#include "jacobi_component/grid.hpp"
#include "jacobi_component/solver.hpp"

#include "common/e2e.hpp"
#include "common/runtime_defaults.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using hpx::program_options::options_description;
using hpx::program_options::value;
using hpx::program_options::variables_map;

using hpx::chrono::high_resolution_timer;

int hpx_main(variables_map& vm)
{
    {
        arts_hpx::run_clock clock;

        std::size_t nx = vm["nx"].as<std::size_t>();
        std::size_t ny = vm["ny"].as<std::size_t>();
        std::size_t max_iterations = vm["max_iterations"].as<std::size_t>();
        std::size_t line_block = vm["line_block"].as<std::size_t>();

        jacobi::grid u(nx, ny, 1.0);

        jacobi::solver solver(u, nx, line_block);

        solver.run(max_iterations);

        arts_hpx::print_e2e(clock);
        if (arts_hpx::struct_marker())
            arts_hpx::print_parcels();
        char line[64];
        std::snprintf(line, sizeof line, "CHECKSUM %.14g\n", solver.checksum());
        arts_hpx::write_stdout_line(line);
    }

    return hpx::finalize();
}

int main(int argc, char** argv)
{
    options_description desc_commandline(
        "usage: " HPX_APPLICATION_STRING " [options]");

    // clang-format off
    desc_commandline.add_options()
        ("output", value<std::string>(), "Output results to file")
        ("nx", value<std::size_t>()->default_value(10),
         "Number of elements in x direction (columns)")
        ("ny", value<std::size_t>()->default_value(10),
         "Number of elements in y direction (rows)")
        ("max_iterations", value<std::size_t>()->default_value(10),
         "Maximum number of iterations")
        ("line_block", value<std::size_t>()->default_value(10),
        "Number of line elements to block the iteration");
    // clang-format on

    arts_hpx::register_geometry_startup();

    hpx::init_params init_args;
    init_args.desc_cmdline = desc_commandline;
    init_args.cfg = arts_hpx::runtime_defaults(false);

    return hpx::init(argc, argv, init_args);
}
#endif
