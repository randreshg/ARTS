//  Copyright (c) 2014 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_EXAMPLES_MINI_GHOST_GLOBAL_SUM_HPP
#define HPX_EXAMPLES_MINI_GHOST_GLOBAL_SUM_HPP

#include <hpx/collectives/broadcast_direct.hpp>
#include <hpx/lcos_local/and_gate.hpp>
#include <hpx/synchronization/spinlock.hpp>

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace mini_ghost {
    template <typename T>
    struct global_sum
    {
    private:
        global_sum(global_sum const&) = delete;
        global_sum& operator=(global_sum const&) = delete;
        typedef hpx::spinlock mutex_type;
        // This gate leaves its mutual exclusion to the code that uses it: the
        // caller holds a lock and hands it in, where the older gate locked
        // internally.  gate_mtx_ below is that lock, and it is not the one
        // that guards the value: the gate fires the waiting future while it is
        // held, and the continuation of that future takes mtx_.
        typedef hpx::lcos::local::and_gate gate_type;

    public:
        global_sum()
          : value_()
          , generation_(0)
        {
        }

        // The gate takes the number of participants at construction, where
        // the older interface took it with every generation.  It is set
        // before any input can arrive, so no participant can find the gate
        // unsized.
        void init(std::size_t count)
        {
            gate_ = gate_type(count);
        }

        global_sum(global_sum &&other)
          : value_(std::move(other.value_))
          , generation_(std::move(other.generation_))
          , gate_(std::move(other.gate_))
        {
        }

        global_sum& operator=(global_sum &&other)
        {
            if(this != &other)
            {
                value_      = std::move(other.value_);
                generation_ = other.generation_;
                gate_       = std::move(other.gate_);
            }
            return *this;
        }

        template<typename Action>
        hpx::future<T>
        add(Action action, std::vector<hpx::id_type> const & ids,
            std::size_t which, T val, std::size_t id, std::size_t idx)
        {
            // The gate no longer advances its generation when a future is
            // taken from it, so this opens the generation explicitly.  The
            // future is taken first: the gate can only fire once every
            // participant has been counted, and this one's own input is sent
            // below.
            hpx::future<void> f;
            {
                std::unique_lock<mutex_type> gl(gate_mtx_);
                f = gate_.get_future(gl);
                generation_ = gate_.next_generation(gl, std::size_t(-1));
            }
            HPX_ASSERT(value_ == 0);

            hpx::lcos::broadcast_post<Action>(ids, generation_, which, val, id, idx);

            return f.then(
                hpx::launch::sync,
                [this](hpx::future<void>) -> T
                {
                    std::lock_guard<mutex_type> l(mtx_);
                    T v = value_; value_ = T(0); return v;
                }
            );
        }

        void set_data(std::size_t generation, std::size_t which, T value)
        {
            std::unique_lock<mutex_type> gl(gate_mtx_);
            gate_.synchronize(generation, gl, "global_sum::set_data");
            {
                std::lock_guard<mutex_type> l(mtx_);
                value_ += value;
            }
            gate_.set(which, gl);     // trigger corresponding and-gate input
        }

    private:
        mutable mutex_type mtx_;
        mutable mutex_type gate_mtx_;

        T value_;
        std::size_t generation_;

        gate_type gate_;        // synchronization gate
    };
}

#endif
