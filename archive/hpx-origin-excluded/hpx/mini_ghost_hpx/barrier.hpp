//  Copyright (c) 2014 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_EXAMPLES_MINI_GHOST_BARRIER_HPP
#define HPX_EXAMPLES_MINI_GHOST_BARRIER_HPP

#include <hpx/include/naming.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/collectives/barrier.hpp>
#include <hpx/runtime_local/shutdown_function.hpp>

#include <cstdint>
#include <memory>

#define HPX_MINI_GHOST_BARRIER "/mini_ghost/barrier"

namespace mini_ghost
{
    namespace detail
    {
        std::unique_ptr<hpx::distributed::barrier>& get_barrier()
        {
            static std::unique_ptr<hpx::distributed::barrier> b;
            return b;
        }
    }

    void barrier_wait()
    {
        // Wait for the barrier to release all localities
        HPX_ASSERT(detail::get_barrier());
        detail::get_barrier()->wait();
    }

    void free_barrier()
    {
        detail::get_barrier().reset();
    }

    void create_barrier()
    {
        // make sure the barrier is released before exiting
        hpx::register_pre_shutdown_function(&mini_ghost::free_barrier);
    }

    void find_barrier()
    {
        // A barrier of this name on every locality: the name is the
        // rendezvous, so no locality has to look the barrier up by symbol.
        std::uint64_t nranks = hpx::get_num_localities(hpx::launch::sync);
        detail::get_barrier().reset(new hpx::distributed::barrier(
            HPX_MINI_GHOST_BARRIER, nranks, hpx::get_locality_id()));
    }
}

#endif
