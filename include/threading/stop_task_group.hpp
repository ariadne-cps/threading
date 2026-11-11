/***************************************************************************
 *            stop_task_group.hpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
/*
 *  This file is part of Ariadne Threading.
 *
 *  Ariadne Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Ariadne Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

/** @file threading/stop_task_group.hpp
 *  @brief Declarations and utilities for stop task group.
 */

#ifndef THREADING_STOP_TASK_GROUP_HPP
#define THREADING_STOP_TASK_GROUP_HPP

#include <functional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include "threading/task_group.hpp"

namespace Ariadne {

//! A scope of long-running jobs that share one cooperative stop request.
//! The owner alone calls fork() and join(). request_stop() may be called
//! concurrently by a job, e.g. when a CDCL search worker in a
//! satisfiability or SMT solver finds a conclusive result.
//! Tasks must check their token; requesting stop never kills a running task.
//! Destroying the scope requests stop and then joins all outstanding jobs.
//! A stop request cannot be reset: create a new group for another run.
template<class Scheduler>
class StopTaskGroup {
  public:
    explicit StopTaskGroup(Scheduler& scheduler) : _group(scheduler) { }

    StopTaskGroup(StopTaskGroup const&) = delete;
    StopTaskGroup& operator=(StopTaskGroup const&) = delete;
    StopTaskGroup(StopTaskGroup&&) = delete;
    StopTaskGroup& operator=(StopTaskGroup&&) = delete;

    ~StopTaskGroup() noexcept { (void)_stop_source.request_stop(); }

    std::stop_token get_stop_token() const noexcept {
        return _stop_source.get_token();
    }

    bool stop_requested() const noexcept {
        return _stop_source.stop_requested();
    }

    bool request_stop() noexcept {
        return _stop_source.request_stop();
    }

    //! Submit one job accepting a std::stop_token. Return a normal TaskHandle.
    template<class F>
    auto fork(F&& function) {
        auto token=get_stop_token();
        return _group.fork([function=std::forward<F>(function),token]() mutable
                -> std::invoke_result_t<std::decay_t<F>&,std::stop_token> {
            return std::invoke(function,token);
        });
    }

    //! Join all jobs, propagating the first exception in submission order.
    //! A stop request is not automatic: request it explicitly when needed.
    void join() { _group.join(); }

  private:
    TaskGroup<Scheduler> _group;
    std::stop_source _stop_source;
};

} // namespace Ariadne

#endif // THREADING_STOP_TASK_GROUP_HPP
