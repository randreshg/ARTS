//  Copyright (c) 2014 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <profiling.hpp>
#include <stepper.hpp>
#include <stencils.hpp>
#include <write_grid.hpp>

#include <hpx/async_combinators/wait_all.hpp>
#include <hpx/async_combinators/when_all.hpp>
#include <hpx/functional/bind.hpp>
#include <hpx/pack_traversal/unwrap.hpp>
#include <hpx/synchronization/spinlock.hpp>
#include <hpx/timing/high_resolution_timer.hpp>

#include <functional>

///////////////////////////////////////////////////////////////////////////////
// Boilerplate code which allows to remotely create and access the stepper
// components
typedef hpx::components::managed_component<
    mini_ghost::stepper<float>
> stepper_float_type;
// The component module the two factory registrations below belong to: a
// program that carries its own components names its own module, and only one
// that borrows the runtime's default name can leave this out.
HPX_REGISTER_COMPONENT_MODULE()
HPX_REGISTER_COMPONENT(stepper_float_type, stepper_float);

typedef hpx::components::managed_component<
    mini_ghost::stepper<double>
> stepper_double_type;
HPX_REGISTER_COMPONENT(stepper_double_type, stepper_double);

// boilerplate code to be able to use set_global_sum_actionas a broadcast action
HPX_REGISTER_BROADCAST_POST_ACTION(
    mini_ghost::stepper<float>::set_global_sum_action
  , mini_ghost_stepper_float_set_global_sum_action)

HPX_REGISTER_BROADCAST_POST_ACTION(
    mini_ghost::stepper<double>::set_global_sum_action
  , mini_ghost_stepper_double_set_global_sum_action)

///////////////////////////////////////////////////////////////////////////////
namespace mini_ghost {

    template <typename Real>
    stepper<Real>::stepper()
      : gen(0)
      , random(0.0, 1.0)
      , rank(-1)
      , init_future_(init_promise_.get_future())
    {}

    template <typename Real>
    hpx::future<void> stepper<Real>::init(params<Real> & p)
    {
        // register our own name
        char const* base_name = "/mini_ghost/stepper/";

        rank = p.rank;

        hpx::register_with_basename(base_name, this->get_id(), p.rank);
        std::vector<hpx::future<hpx::id_type> > steppers =
            hpx::find_all_from_basename(base_name, p.nranks);

        return hpx::when_all(steppers).then(hpx::unwrapping_n<2>(
            [this, &p](std::vector<hpx::id_type> && ids)
            {
                stepper_ids = std::move(ids);

                // creating a mapping table to favorably distribute the indices
                /*
                std::vector<std::size_t> id_map(p.nranks);
                std::size_t r = p.rank % p.nranks;
                for (std::size_t i = 0; i != p.nranks; ++i)
                {
                    id_map[r] = i;
                    r = (r + 1) % p.nranks;
                }
                */

                // Set position in 3D processor grid
                std::size_t myrank_xy = p.rank % (p.npx*p.npy);
                std::size_t remainder = myrank_xy % p.npx;

                std::size_t my_px = remainder;
                std::size_t my_py = myrank_xy / p.npx;
                std::size_t my_pz = p.rank / (p.npx*p.npy);

                // Initialize grids and spikes
                auto random_lambda = [this]() -> Real { return random(gen); };
                std::size_t num_sum_grid = 0;

                // create partition objects, one for each variable
                std::vector<hpx::future<void> > partition_initialized;
                partition_initialized.reserve(p.num_vars);

                for(std::size_t i=0; i < p.num_vars; ++i)
                {
                    partition_initialized.push_back(hpx::future<void>());
                }

                spikes_.reserve(p.num_vars);
                partitions_.reserve(p.num_vars);
                for(std::size_t var = 0; var != p.num_vars; ++var)
                {
                    spikes_.push_back(
                        spikes<Real>(p, my_px, my_py, my_pz, random_lambda)
                    );

                    // Constructed in place: the partition attaches a
                    // continuation that captures its own address to receive
                    // the global sum of its initial grid, so an object that
                    // is constructed and then moved here would leave that
                    // continuation writing to the object it was moved out of.
                    // The vector's storage is reserved above, so the address
                    // stays valid for the rest of the run.
                    partitions_.emplace_back(
                            var, p
                          , stepper_ids//, id_map
                          , my_px, my_py, my_pz
                          , random_lambda
                          , partition_initialized[var]
                    );

                    if (partitions_.back().sum_grid())
                        ++num_sum_grid;
                }

                // wait for all local partitions to be initialized
                init_promise_.set_value();
                hpx::wait_all(partition_initialized);

                //FIXME: setup performance captures...

                if(p.rank == 0)
                {
                    p.print_header(num_sum_grid);
                }
            }
        ));
    }

    template <typename Real>
    void stepper<Real>::run(std::size_t num_spikes, std::size_t num_tsteps)
    {
        hpx::chrono::high_resolution_timer timer_all;

        hpx::spinlock io_mutex;

        for(std::size_t spike = 0; spike != num_spikes; ++spike)
        {
            std::vector<hpx::future<void> > run_futures;
            run_futures.reserve(partitions_.size());
            for(auto & partition : partitions_)
            {
                partition.insert_spike(spikes_[spike], spike);
                run_futures.push_back(
                    hpx::async(
                        hpx::bind(
                            &partition_type::run
                          , std::ref(partition)
                          , std::ref(stepper_ids)
                          , num_tsteps
                          , std::ref(io_mutex)
                        )
                    )
                );
            }
            hpx::wait_all(run_futures);
        }
        profiling::data().time_wall(timer_all.elapsed());
    }

    ///////////////////////////////////////////////////////////////////////////
    template <typename Real>
    void stepper<Real>::set_north_zone(buffer_type buffer, std::size_t step,
        std::size_t var)
    {
        init_future_.wait();
        partitions_[var].recv_buffer_north_.set_buffer(buffer, step);
    }

    template <typename Real>
    void stepper<Real>::set_south_zone(buffer_type buffer, std::size_t step,
        std::size_t var)
    {
        init_future_.wait();
        partitions_[var].recv_buffer_south_.set_buffer(buffer, step);
    }

    template <typename Real>
    void stepper<Real>::set_east_zone(buffer_type buffer, std::size_t step,
        std::size_t var)
    {
        init_future_.wait();
        partitions_[var].recv_buffer_east_.set_buffer(buffer, step);
    }

    template <typename Real>
    void stepper<Real>::set_west_zone(buffer_type buffer, std::size_t step,
        std::size_t var)
    {
        init_future_.wait();
        partitions_[var].recv_buffer_west_.set_buffer(buffer, step);
    }

    template <typename Real>
    void stepper<Real>::set_front_zone(buffer_type buffer, std::size_t step,
        std::size_t var)
    {
        init_future_.wait();
        partitions_[var].recv_buffer_front_.set_buffer(buffer, step);
    }

    template <typename Real>
    void stepper<Real>::set_back_zone(buffer_type buffer, std::size_t step,
        std::size_t var)
    {
        init_future_.wait();
        partitions_[var].recv_buffer_back_.set_buffer(buffer, step);
    }

    template <typename Real>
    void stepper<Real>::set_global_sum(std::size_t generation, std::size_t which,
        Real value, std::size_t idx, std::size_t id)
    {
        init_future_.wait();
        HPX_ASSERT(which < stepper_ids.size());
        partitions_[idx].sum_allreduce(id, generation, which, value);
    }

    // explicitly instantiate steppers
    template struct stepper<float>;
    template struct stepper<double>;
}
